#include "serialreader.h"

#include <QSerialPortInfo>
#include <algorithm>
#include <cstdlib>

SerialReader::SerialReader(QObject *parent)
    : QObject(parent)
{
    for (int i = 0; i < kSize * kSize; ++i) {
        m_distances.append(-1);
        m_kinds.append(NoTarget);
        m_lastShown.append(-1);
    }

    m_port.setBaudRate(460800);
    connect(&m_port, &QSerialPort::readyRead, this, &SerialReader::onReadyRead);
    connect(&m_port, &QSerialPort::errorOccurred, this, [this](QSerialPort::SerialPortError e) {
        if (e == QSerialPort::ResourceError) // device unplugged
            close();
    });

    refreshPorts();
}

void SerialReader::refreshPorts()
{
    m_ports.clear();
    for (const QSerialPortInfo &info : QSerialPortInfo::availablePorts())
        m_ports.append(info.portName());
    emit portsChanged();
}

void SerialReader::open(const QString &portName)
{
    close();
    m_port.setPortName(portName);
    if (m_port.open(QIODevice::ReadOnly)) {
        // The ESP32's USB-Serial/JTAG resets on DTR/RTS combinations, so set
        // RTS low before raising DTR.
        m_port.setRequestToSend(false);
        m_port.setDataTerminalReady(true);
        m_sinceLastLine.start();
    }
    emit connectedChanged();
}

void SerialReader::close()
{
    if (m_port.isOpen()) {
        // Drop RTS then DTR ourselves so the lines never pass through a
        // reset/download-mode combination while the driver releases them.
        m_port.setRequestToSend(false);
        m_port.setDataTerminalReady(false);
        m_port.close();
    }
    m_buffer.clear();
    m_frame.clear();
    m_lastShown.fill(-1);
    setEventStatus(-1);
    emit connectedChanged();
}

void SerialReader::setEventStatus(int status)
{
    if (status == m_eventStatus)
        return;
    m_eventStatus = status;
    emit eventStatusChanged();
}

void SerialReader::onReadyRead()
{
    m_buffer.append(m_port.readAll());

    qsizetype nl;
    while ((nl = m_buffer.indexOf('\n')) >= 0) {
        handleLine(m_buffer.left(nl).trimmed());
        m_buffer.remove(0, nl + 1);
    }
}

void SerialReader::handleLine(const QByteArray &line)
{
    // Frames aren't delimited, so a pause between lines marks the start of a new frame.
    if (m_sinceLastLine.restart() > kFrameGapMs)
        m_frame.clear();

    const QList<QByteArray> tokens = line.simplified().split(' ');

    // A lone integer is the event status line that starts each frame.
    if (tokens.size() == 1) {
        bool ok = false;
        const int status = tokens[0].toInt(&ok);
        if (ok) {
            setEventStatus(status);
            m_frame.clear();
            return;
        }
    }

    if (tokens.size() != kSize) {
        m_frame.clear(); // status message or partial line
        return;
    }

    for (const QByteArray &t : tokens) {
        bool ok = false;
        if (t.startsWith('E')) {
            const int mm = t.mid(1).toInt(&ok);
            m_frame.append(ok ? Zone{ mm, UncertainKept } : Zone{ -1, NoTarget });
        } else {
            const int mm = t.toInt(&ok); // "X" fails to parse -> no target
            m_frame.append(ok ? Zone{ mm, Valid } : Zone{ -1, NoTarget });
        }
    }

    if (m_frame.size() == kSize * kSize) {
        processFrame();
        m_frame.clear();
    }
}

void SerialReader::processFrame()
{
    auto near = [](int a, int b) {
        const int tol = std::max(kMinToleranceMm, int(kToleranceRatio * b));
        return std::abs(a - b) <= tol;
    };

    QList<Zone> out = m_frame;

    // Uncertain ("E") zones: keep the sensor value if it agrees with the
    // neighbours or the last frame, otherwise infer one.
    for (int r = 0; r < kSize; ++r) {
        for (int c = 0; c < kSize; ++c) {
            Zone &z = out[r * kSize + c];
            if (z.kind != UncertainKept)
                continue;

            // Spatial estimate: median of valid neighbours in this frame.
            QList<int> neighbours;
            for (int dr = -1; dr <= 1; ++dr) {
                for (int dc = -1; dc <= 1; ++dc) {
                    const int nr = r + dr, nc = c + dc;
                    if ((dr || dc) && nr >= 0 && nr < kSize && nc >= 0 && nc < kSize) {
                        const Zone &n = m_frame[nr * kSize + nc];
                        if (n.kind == Valid)
                            neighbours.append(n.mm);
                    }
                }
            }
            int spatial = -1;
            if (neighbours.size() >= 2) {
                std::sort(neighbours.begin(), neighbours.end());
                spatial = neighbours[neighbours.size() / 2];
            }

            // Temporal estimate: what this cell showed last frame.
            const int temporal = m_lastShown[r * kSize + c];

            if (spatial < 0 && temporal < 0)
                continue; // nothing to judge against; keep the sensor value

            const bool fitsSpatial = spatial >= 0 && near(z.mm, spatial);
            const bool fitsTemporal = temporal >= 0 && near(z.mm, temporal);
            if (fitsSpatial || fitsTemporal)
                continue; // consistent with surroundings or history; keep it

            z.mm = spatial >= 0 ? spatial : temporal;
            z.kind = Inferred;
        }
    }

    // No-target (255) zones are always filled in. Only sensor readings that
    // survived the pass above are used, so inferred values don't chain.
    auto isReading = [](const Zone &z) { return z.kind == Valid || z.kind == UncertainKept; };
    const QList<Zone> readings = out;

    for (int r = 0; r < kSize; ++r) {
        for (int c = 0; c < kSize; ++c) {
            Zone &z = out[r * kSize + c];
            if (z.kind != NoTarget)
                continue;

            // 1. Median of neighbouring readings.
            QList<int> neighbours;
            for (int dr = -1; dr <= 1; ++dr) {
                for (int dc = -1; dc <= 1; ++dc) {
                    const int nr = r + dr, nc = c + dc;
                    if ((dr || dc) && nr >= 0 && nr < kSize && nc >= 0 && nc < kSize) {
                        const Zone &n = readings[nr * kSize + nc];
                        if (isReading(n))
                            neighbours.append(n.mm);
                    }
                }
            }
            if (!neighbours.isEmpty()) {
                std::sort(neighbours.begin(), neighbours.end());
                z = { neighbours[neighbours.size() / 2], Inferred };
                continue;
            }

            // 2. What this cell showed last frame.
            const int temporal = m_lastShown[r * kSize + c];
            if (temporal >= 0) {
                z = { temporal, Inferred };
                continue;
            }

            // 3. Inverse-distance-weighted average of every reading in the frame.
            double sum = 0, weights = 0;
            for (int i = 0; i < readings.size(); ++i) {
                if (!isReading(readings[i]))
                    continue;
                const int dr = i / kSize - r, dc = i % kSize - c;
                const double w = 1.0 / (dr * dr + dc * dc);
                sum += w * readings[i].mm;
                weights += w;
            }
            if (weights > 0)
                z = { int(sum / weights + 0.5), Inferred };
            // Otherwise the whole frame is empty and the zone stays NoTarget.
        }
    }

    m_distances.clear();
    m_kinds.clear();
    for (int i = 0; i < out.size(); ++i) {
        m_distances.append(out[i].mm);
        m_kinds.append(out[i].kind);
        m_lastShown[i] = out[i].mm;
    }
    emit distancesChanged();
}

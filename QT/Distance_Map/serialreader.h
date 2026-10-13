#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QSerialPort>
#include <QStringList>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

// Reads 8x8 distance frames from the ESP32.
// Format: one event status line (a single integer bitmask, see EventFlag),
// then 8 lines, each with 8 space-separated tokens:
//   "1234"  valid reading in mm (status 5 or 9)
//   "E1234" uncertain reading in mm (any other status)
//   "X"     no target (status 255)
class SerialReader : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QStringList ports READ ports NOTIFY portsChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    Q_PROPERTY(QVariantList distances READ distances NOTIFY distancesChanged)
    Q_PROPERTY(QVariantList kinds READ kinds NOTIFY distancesChanged)
    Q_PROPERTY(int eventStatus READ eventStatus NOTIFY eventStatusChanged)

public:
    // How each displayed value was arrived at.
    enum Kind { Valid, UncertainKept, Inferred, NoTarget };
    Q_ENUM(Kind)

    // Bits of eventStatus; matches the firmware's Events enum. 0 = clear, -1 = not yet received.
    // ClearButMotorDisabled: no fault, but the motor stays off until the speed input returns to zero.
    enum EventFlag { TimeoutEvent = 1, UnderThresholdEvent = 2, ClearButMotorDisabled = 4 };
    Q_ENUM(EventFlag)

    explicit SerialReader(QObject *parent = nullptr);

    QStringList ports() const { return m_ports; }
    bool connected() const { return m_port.isOpen(); }
    int eventStatus() const { return m_eventStatus; }
    QVariantList distances() const { return m_distances; }
    QVariantList kinds() const { return m_kinds; }

    Q_INVOKABLE void refreshPorts();
    Q_INVOKABLE void open(const QString &portName);
    Q_INVOKABLE void close();

signals:
    void portsChanged();
    void connectedChanged();
    void distancesChanged();
    void eventStatusChanged();

private:
    struct Zone { int mm; Kind kind; }; // mm = -1 for NoTarget

    void onReadyRead();
    void handleLine(const QByteArray &line);
    void processFrame();
    void setEventStatus(int status);

    static constexpr int kSize = 8;
    static constexpr qint64 kFrameGapMs = 20; // lines further apart than this start a new frame
    static constexpr int kMinToleranceMm = 100;
    static constexpr double kToleranceRatio = 0.10;

    QSerialPort m_port;
    QByteArray m_buffer;
    QList<Zone> m_frame;     // zones collected so far for the current frame
    QList<int> m_lastShown;  // value shown per cell last frame, -1 if none
    QElapsedTimer m_sinceLastLine;
    QStringList m_ports;
    QVariantList m_distances;
    QVariantList m_kinds;
    int m_eventStatus = -1;
};

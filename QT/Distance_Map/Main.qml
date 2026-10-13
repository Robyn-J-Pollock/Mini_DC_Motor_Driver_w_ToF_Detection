import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    width: 640
    height: 720
    visible: true
    title: qsTr("Distance Map")
    color: "#202020"

    // Distance range mapped onto the colour scale. Closer = higher "elevation".
    readonly property int minMm: 0
    readonly property int maxMm: 3000

    // Hypsometric tints, low to high elevation.
    readonly property var stops: [
        { t: 0.00, c: Qt.rgba(0.00, 0.38, 0.19, 1) }, // deep green
        { t: 0.20, c: Qt.rgba(0.40, 0.66, 0.31, 1) }, // green
        { t: 0.40, c: Qt.rgba(0.93, 0.89, 0.40, 1) }, // yellow
        { t: 0.60, c: Qt.rgba(0.96, 0.58, 0.20, 1) }, // orange
        { t: 0.80, c: Qt.rgba(0.86, 0.15, 0.12, 1) }, // red
        { t: 1.00, c: Qt.rgba(0.50, 0.04, 0.06, 1) }  // dark red
    ]

    function tint(mm) {
        if (mm < 0)
            return "#555555" // no target
        const t = 1 - Math.max(0, Math.min(1, (mm - minMm) / (maxMm - minMm)))
        for (let i = 1; i < stops.length; ++i) {
            if (t <= stops[i].t) {
                const a = stops[i - 1], b = stops[i]
                const f = (t - a.t) / (b.t - a.t)
                return Qt.rgba(a.c.r + (b.c.r - a.c.r) * f,
                               a.c.g + (b.c.g - a.c.g) * f,
                               a.c.b + (b.c.b - a.c.b) * f, 1)
            }
        }
        return stops[stops.length - 1].c
    }

    SerialReader { id: reader }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 12

        RowLayout {
            ComboBox {
                id: portBox
                model: reader.ports
                enabled: !reader.connected
                Layout.preferredWidth: 160
            }
            Button {
                text: qsTr("Refresh")
                enabled: !reader.connected
                onClicked: reader.refreshPorts()
            }
            Button {
                text: reader.connected ? qsTr("Disconnect") : qsTr("Connect")
                enabled: reader.connected || portBox.currentText !== ""
                onClicked: reader.connected ? reader.close() : reader.open(portBox.currentText)
            }

            Item { Layout.fillWidth: true }

            // Motor driver event status reported by the ESP32 at the start of each frame
            Rectangle {
                id: statusBadge
                readonly property int status: reader.eventStatus
                readonly property var reasons: {
                    let r = []
                    if (status > 0 && (status & SerialReader.TimeoutEvent))
                        r.push(qsTr("Sensor timeout"))
                    if (status > 0 && (status & SerialReader.UnderThresholdEvent))
                        r.push(qsTr("Too close"))
                    return r
                }
                // Clear of faults, but waiting for the speed input to return to zero
                readonly property bool waitingForZero: status === SerialReader.ClearButMotorDisabled
                implicitWidth: statusText.implicitWidth + 20
                implicitHeight: statusText.implicitHeight + 10
                radius: height / 2
                color: status < 0 ? "#555555"
                       : status === 0 ? "#2e7d32"
                       : waitingForZero ? "#f9a825"
                       : "#c62828"

                Label {
                    id: statusText
                    anchors.centerIn: parent
                    color: statusBadge.waitingForZero ? "black" : "white"
                    font.bold: true
                    text: statusBadge.status < 0 ? qsTr("Status: —")
                          : statusBadge.status === 0 ? qsTr("Running")
                          : statusBadge.waitingForZero ? qsTr("Ready: turn speed to 0 to start")
                          : qsTr("Stopped: ") + statusBadge.reasons.join(", ")
                }
            }
        }

        Grid {
            id: grid
            columns: 8
            Layout.fillWidth: true
            Layout.fillHeight: true
            readonly property real cell: Math.min(width, height) / 8

            Repeater {
                model: 64
                Rectangle {
                    required property int index
                    readonly property int mm: reader.distances[index]
                    readonly property int kind: reader.kinds[index]
                    width: grid.cell
                    height: grid.cell
                    color: tint(mm)

                    Text {
                        anchors.centerIn: parent
                        // ~ = inferred from neighbours/last frame, italic = uncertain reading kept
                        text: parent.kind === SerialReader.NoTarget ? "X"
                              : parent.kind === SerialReader.Inferred ? "~" + parent.mm
                              : parent.mm
                        font.italic: parent.kind === SerialReader.UncertainKept
                        // Light text on dark tints, dark text on light ones
                        color: parent.color.hslLightness < 0.45 ? "white" : "black"
                        opacity: 0.7
                        font.pixelSize: grid.cell * 0.22
                    }
                }
            }
        }

        // Legend
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            Rectangle {
                Layout.fillWidth: true
                height: 16
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0.0; color: stops[5].c }
                    GradientStop { position: 0.2; color: stops[4].c }
                    GradientStop { position: 0.4; color: stops[3].c }
                    GradientStop { position: 0.6; color: stops[2].c }
                    GradientStop { position: 0.8; color: stops[1].c }
                    GradientStop { position: 1.0; color: stops[0].c }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Label { text: minMm + " mm"; color: "white" }
                Item { Layout.fillWidth: true }
                Label { text: maxMm + " mm"; color: "white" }
            }
        }
    }
}

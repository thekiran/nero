import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    visible: true
    width: 1040
    height: 700
    minimumWidth: 720
    minimumHeight: 480
    title: qsTr("NERO — Network Research Suite")
    color: theme.background

    // Centralized design tokens (dark-first workstation palette).
    QtObject {
        id: theme
        readonly property color background: "#0B1018"
        readonly property color sidebar: "#101722"
        readonly property color surface: "#162231"
        readonly property color elevated: "#1D2B3C"
        readonly property color border: "#29394B"
        readonly property color primary: "#3B9EFF"
        readonly property color cyan: "#42D4E6"
        readonly property color success: "#45C99C"
        readonly property color warning: "#F4B860"
        readonly property color error: "#EF6A73"
        readonly property color text: "#EAF2FF"
        readonly property color textDim: "#96A8BD"
    }

    property var navItems: [
        "Overview", "Network Interfaces", "Port Scanner", "Wi-Fi Monitor",
        "Access Points", "Clients", "Signal Analysis", "PCAP Analyzer",
        "Statistics", "Sessions", "Logs", "Settings"
    ]
    property int currentIndex: 0

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ---- Sidebar ----
        Rectangle {
            Layout.preferredWidth: 220
            Layout.fillHeight: true
            color: theme.sidebar
            border.color: theme.border
            border.width: 0

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 0
                spacing: 0

                Item { Layout.fillWidth: true; implicitHeight: 64
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        x: 20
                        text: "NERO"
                        color: theme.text
                        font.pixelSize: 22
                        font.letterSpacing: 4
                        font.bold: true
                    }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: theme.border }

                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: root.navItems
                    delegate: Rectangle {
                        width: ListView.view.width
                        height: 40
                        color: index === root.currentIndex ? theme.surface : "transparent"
                        Rectangle {
                            anchors.left: parent.left
                            width: 3; height: parent.height
                            color: index === root.currentIndex ? theme.primary : "transparent"
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            x: 20
                            text: modelData
                            color: index === root.currentIndex ? theme.text : theme.textDim
                            font.pixelSize: 14
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.currentIndex = index
                        }
                    }
                }

                Rectangle { Layout.fillWidth: true; height: 1; color: theme.border }
                Item {
                    Layout.fillWidth: true; implicitHeight: 54
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 20
                        anchors.rightMargin: 20
                        Rectangle { width: 9; height: 9; radius: 5; color: theme.success }
                        Text { text: "Core: " + nero.version; color: theme.textDim; font.pixelSize: 12 }
                        Item { Layout.fillWidth: true }
                    }
                }
            }
        }

        // ---- Content ----
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // Header / toolbar
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 56
                color: theme.background
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 24
                    anchors.rightMargin: 24
                    Text {
                        text: root.navItems[root.currentIndex]
                        color: theme.text
                        font.pixelSize: 20
                        font.bold: true
                    }
                    Item { Layout.fillWidth: true }
                    Rectangle {
                        implicitWidth: ver.implicitWidth + 24
                        implicitHeight: 26
                        radius: 13
                        color: theme.elevated
                        border.color: theme.border
                        Text {
                            id: ver
                            anchors.centerIn: parent
                            text: "nero-gui · v" + nero.version
                            color: theme.cyan
                            font.pixelSize: 12
                        }
                    }
                }
            }
            Rectangle { Layout.fillWidth: true; height: 1; color: theme.border }

            // Overview page (real data); other pages show an honest placeholder.
            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: root.currentIndex === 0 ? 0 : 1

                // -- Overview --
                Item {
                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 24
                        spacing: 20

                        // Metric cards row
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 16
                            Repeater {
                                model: [
                                    { label: "CORE VERSION", value: nero.version, accent: theme.primary },
                                    { label: "FRONTEND", value: "Desktop (Qt 6)", accent: theme.cyan },
                                    { label: "SHARED ENGINE", value: "C11 core", accent: theme.success }
                                ]
                                delegate: Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 86
                                    radius: 10
                                    color: theme.surface
                                    border.color: theme.border
                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 16
                                        spacing: 6
                                        Text { text: modelData.label; color: theme.textDim; font.pixelSize: 11; font.letterSpacing: 1 }
                                        Text { text: modelData.value; color: modelData.accent; font.pixelSize: 22; font.bold: true }
                                    }
                                }
                            }
                        }

                        // Diagnostics panel (real nero_doctor output from the core)
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            radius: 10
                            color: theme.surface
                            border.color: theme.border
                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 16
                                spacing: 10
                                RowLayout {
                                    Layout.fillWidth: true
                                    Text { text: "SYSTEM DIAGNOSTICS"; color: theme.text; font.pixelSize: 13; font.bold: true }
                                    Item { Layout.fillWidth: true }
                                    Text { text: "source: nero_doctor() · read-only"; color: theme.textDim; font.pixelSize: 11 }
                                }
                                Rectangle { Layout.fillWidth: true; height: 1; color: theme.border }
                                ScrollView {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    clip: true
                                    TextArea {
                                        id: doctorText
                                        readOnly: true
                                        wrapMode: TextArea.NoWrap
                                        selectByMouse: true
                                        color: theme.text
                                        font.family: "Menlo, monospace"
                                        font.pixelSize: 12
                                        background: null
                                        text: nero.doctorReport()
                                    }
                                }
                            }
                        }
                    }
                }

                // -- Placeholder for not-yet-built pages (honest, no fake data) --
                Item {
                    ColumnLayout {
                        anchors.centerIn: parent
                        spacing: 10
                        Text {
                            Layout.alignment: Qt.AlignHCenter
                            text: root.navItems[root.currentIndex]
                            color: theme.text
                            font.pixelSize: 20
                            font.bold: true
                        }
                        Text {
                            Layout.alignment: Qt.AlignHCenter
                            text: "This view is not implemented yet. No sample data is shown."
                            color: theme.textDim
                            font.pixelSize: 13
                        }
                    }
                }
            }
        }
    }
}

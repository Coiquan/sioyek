
import QtQuick 2.2
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.1

Item{
    id: root
    property list<color> colors;

    signal colorClicked(int index)
    signal colorHeld(int index)

    Repeater{
        model: root.colors.length

        Rectangle{
            required property int index
            anchors.top: get_top_anchor()
            anchors.bottom: get_bottom_anchor()

            width: is_too_small() ? parent.width / Math.ceil(root.colors.length / 2) : parent.width / root.colors.length
            x: is_too_small() ?  (visible ? (index % Math.ceil(root.colors.length / 2)) * width : root.width / 2 - width / 2) : (visible ? index * width : root.width / 2 - width / 2)
            color: root.colors[index]

            function get_top_anchor(){
                if (is_too_small()){
                    if (index < Math.ceil(root.colors.length / 2)){
                        return parent.top;
                    }
                    else{
                        return parent.verticalCenter;
                    }
                }
                else{
                    return parent.top;
                }
            }
            function get_bottom_anchor(){
                if (is_too_small()){
                    if (index < Math.ceil(root.colors.length / 2)){
                        return parent.verticalCenter;
                    }
                    else{
                        return parent.bottom;
                    }
                }
                else{
                    return parent.bottom;
                }

            }
            function is_too_small(){
                return root.width / root.colors.length < 20;
            }

            Behavior on x {

                NumberAnimation {
                    duration: 500
                    easing.type: Easing.OutExpo
                }
            }

            Text{
                anchors.centerIn: parent
                text: String.fromCharCode(97 + index)
                color: root.colors[index].hslLightness > 0.5 ? "black" : "white"
            }
            MouseArea {
                anchors.fill: parent
                onClicked: {
                    root.colorClicked(index);
                }
                onPressAndHold: {
                    root.colorHeld(index);
                }
            }
        }

    }
}

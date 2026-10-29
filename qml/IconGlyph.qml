import QtQuick

Canvas {
    id: icon
    property string name: ""
    property color color: "#aaaaaa"
    implicitWidth: 18; implicitHeight: 18
    onAvailableChanged: if(available) requestPaint()
    Component.onCompleted: requestPaint()
    onNameChanged: requestPaint()
    onColorChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()
    onPaint: {
        var c=getContext("2d"); c.reset(); c.scale(width/24,height/24)
        c.strokeStyle=color; c.lineWidth=1.7; c.lineCap="round"; c.lineJoin="round"
        function line(x1,y1,x2,y2){c.moveTo(x1,y1);c.lineTo(x2,y2)}
        c.beginPath()
        if(name==="clock") { c.lineWidth=2;c.arc(12,12,10,0,Math.PI*2);line(12,6,12,12);line(12,12,16,14) }
        else if(name==="home") { c.rect(2,7,20,14); line(7,2,12,7);line(12,7,17,2) }
        else if(name==="queue" || name==="segments") { for(var y=6;y<=18;y+=6){line(8,y,21,y);line(3,y,4,y)} }
        else if(name==="history") { c.arc(12,12,9,-2.4,3.9);line(3,3,3,9);line(3,9,9,9);line(12,7,12,12);line(12,12,16,14) }
        else if(name==="settings") {
            for(var i=0;i<24;i++){ var r=(i%3===0 || i%3===1)?10:8; var a=i*Math.PI/12; if(i===0)c.moveTo(12+r*Math.cos(a),12+r*Math.sin(a));else c.lineTo(12+r*Math.cos(a),12+r*Math.sin(a)) }
            c.closePath();c.moveTo(15.5,12);c.arc(12,12,3.5,0,Math.PI*2)
        }
        else if(name==="media") { c.rect(3,3,18,7);c.rect(3,14,18,7);line(6,6,7,6);line(6,17,7,17);line(11,6,18,6);line(11,17,18,17) }
        else if(name==="application") { for(var j=0;j<3;j++){var x=5+j*7;line(x,3,x,21);c.rect(x-2,j===1?13:6,4,4)} }
        else if(name==="endpoints") {line(4,20,20,4);line(10,4,20,4);line(20,4,20,14)}
        else if(name==="notifications") {c.moveTo(4,17);c.lineTo(6,14);c.lineTo(6,9);c.arc(12,9,6,Math.PI,0);c.lineTo(18,14);c.lineTo(20,17);c.closePath();c.moveTo(10,20);c.arc(12,20,2,Math.PI,0,true)}
        c.stroke()
    }
}

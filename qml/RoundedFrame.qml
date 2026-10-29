import QtQuick

// Canvas corner coverage also works with Qt Quick's software renderer. A
// Rectangle's clip flag alone does not clip children to its rounded outline.
Canvas {
    id: frame
    property real radius: 12
    property color outsideColor: "#1a1a1f"
    property color borderColor: "transparent"
    property real borderWidth: 1
    property bool coverCorners: true
    property real edgeSoftness: 0
    antialiasing: true
    onAvailableChanged: if (available) requestPaint()
    Component.onCompleted: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()
    onRadiusChanged: requestPaint()
    onCoverCornersChanged: requestPaint()
    onEdgeSoftnessChanged: requestPaint()
    onOutsideColorChanged: requestPaint()
    onBorderColorChanged: requestPaint()
    onPaint: {
        var c=getContext("2d"); c.reset()
        var r=Math.min(radius,width/2,height/2)
        c.fillStyle=outsideColor
        function corner(x,y,rotation) {
            c.save();c.translate(x,y);c.rotate(rotation)
            c.beginPath();c.moveTo(0,0);c.lineTo(r,0)
            c.arc(r,r,r,-Math.PI/2,-Math.PI,true);c.closePath();c.fill();c.restore()
        }
        if(coverCorners) {
            corner(0,0,0);corner(width,0,Math.PI/2)
            corner(width,height,Math.PI);corner(0,height,-Math.PI/2)
        }
        // Fade only the inner edge into its surroundings. Keep the artwork
        // itself sharp and leave GPU corner clipping to the existing mask.
        for(var inset=0.5;inset<edgeSoftness;inset+=0.5) {
            var q=Math.max(0,r-inset)
            c.globalAlpha=0.28*Math.pow(1-inset/edgeSoftness,2)
            c.strokeStyle=outsideColor;c.lineWidth=1
            c.beginPath();c.moveTo(r,inset);c.lineTo(width-r,inset)
            c.arc(width-r,r,q,-Math.PI/2,0);c.lineTo(width-inset,height-r)
            c.arc(width-r,height-r,q,0,Math.PI/2);c.lineTo(r,height-inset)
            c.arc(r,height-r,q,Math.PI/2,Math.PI);c.lineTo(inset,r)
            c.arc(r,r,q,Math.PI,Math.PI*1.5);c.closePath();c.stroke()
        }
        c.globalAlpha=1
        if(borderWidth>0) {
            var d=borderWidth/2; var q=Math.max(0,r-d)
            c.strokeStyle=borderColor;c.lineWidth=borderWidth
            c.beginPath();c.moveTo(r,d);c.lineTo(width-r,d)
            c.arc(width-r,r,q,-Math.PI/2,0);c.lineTo(width-d,height-r)
            c.arc(width-r,height-r,q,0,Math.PI/2);c.lineTo(r,height-d)
            c.arc(r,height-r,q,Math.PI/2,Math.PI);c.lineTo(d,r)
            c.arc(r,r,q,Math.PI,Math.PI*1.5);c.closePath();c.stroke()
        }
    }
}

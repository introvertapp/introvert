import QtQuick
import QtQuick.Window
Image {
    smooth: true
    mipmap: true
    antialiasing: true
    asynchronous: true
    retainWhileLoading: true
    readonly property bool svgSource: source.toString().split(/[?#]/)[0].toLowerCase().endsWith(".svg")
    // Use an integer multiple of the physical display size. Decoding only
    // slightly above it forces another resampling pass and loses fine detail.
    // UI SVGs fit within 32 logical pixels. Decode them at a stable, oversampled
    // height and preserve their intrinsic aspect ratio. Deriving their decode
    // size from layout width feeds implicit image size back into that layout.
    sourceSize.width: svgSource ? -1 : Math.max(1, Math.ceil(width * Screen.devicePixelRatio * 2))
    sourceSize.height: svgSource ? Math.ceil(32 * Screen.devicePixelRatio * 2) : Math.max(1, Math.ceil(height * Screen.devicePixelRatio * 2))
}

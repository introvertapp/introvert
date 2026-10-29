.pragma library
function seconds(value) {
    var rounded=Math.round(Math.max(0,Number(value)||0)*1000)/1000
    var tenths=Math.floor(rounded*10)/10
    return Math.floor(rounded*10)%10 ? tenths.toFixed(1) : String(Math.floor(tenths))
}
function mmss(value) {
    var tenths=Math.max(0,Math.floor((Number(value)||0)*10)), whole=Math.floor(tenths/10)
    var base=String(Math.floor(whole/60)).padStart(2,"0")+":"+String(whole%60).padStart(2,"0")
    return base+(tenths%10 ? "."+(tenths%10) : "")
}
function lines(start,end,format) {
    var duration=seconds(end-start)+"s", rows=[]
    if(format!=="seconds") rows.push(mmss(start)+"m → "+mmss(end)+"m ("+duration+")")
    if(format!=="MM:SS") rows.push(seconds(start)+"s → "+seconds(end)+"s ("+duration+")")
    return rows.join("\n")
}

.pragma library

function name(domain) {
    return ({"introdb.app": "IntroDB", "theintrodb.org": "TheIntroDB", "skipdb.tv": "SkipDB"})[domain] || domain
}

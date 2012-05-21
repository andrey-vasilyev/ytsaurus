// These functions are mainly from Express framework.
// http://expressjs.com/
////////////////////////////////////////////////////////////////////////////////

/**
 * Check if `type` matches given `str`.
 */
exports.is = function(type, str) {
    if (!str) {
        return false;
    }

    str = str.split(";")[0];

    if (type.indexOf("*") != -1) {
        type = type.split("/");
        str = str.split("/");

        return (type[0] == "*" && type[1] == str[1])
            || (type[1] == "*" && type[0] == str[0])
            || (type[0] == "*" && type[1] == "*");
    }

    return type == str;
}


/**
 * Check if `type(s)` are acceptable based on the given `str`.
 */
exports.accepts = function(type, str) {
    if (!str) {
        return false;
    }

    var accepted = exports.parseAccept(str);
    for (var i = 0, imax = accepted.length; i < imax; ++i) {
        if (exports.testAccept(type, accepted[i])) {
            return true;
        }
    }

    return false;
};

/**
 * Parse Accept `str`, returning an array objects containing
 * `.type` and `.subtype` along with the values provided by
 * `parseQuality()`.
 */

exports.parseAccept = function(str) {
    return str
        .split(/ *, */)
        .map(exports.parseQuality)
        .filter(function(obj) {
            return obj.quality;
        })
        .sort(function(a, b) {
            return b.quality - a.quality;
        })
        .map(function(obj) {
            var parts = obj.value.split("/");
            obj.type = parts[0];
            obj.subtype = parts[1];
            return obj;
        });
};

/**
 * Parse quality `str`, returning an object with `.value` and `.quality`.
 */

exports.parseQuality = function(str) {
    var parts = str.split(/ *; */);
    var value = parts[0];

    var quality = parts[1] ? parseFloat(parts[1].split(/ *= */)[1]) : 1.0;

    return { value: value, quality: quality };
}

/**
 * Check if `type` array is acceptable for `other`.
 */

exports.testAccept = function(type, other) {
    var parts = type.split("/");
    return (parts[0] == other.type || "*" == other.type)
        && (parts[1] == other.subtype || "*" == other.subtype);
};


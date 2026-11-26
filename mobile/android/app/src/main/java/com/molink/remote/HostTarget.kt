package com.molink.remote

import android.net.Uri

/** A validated `host:port` target for one remote moLink core. */
data class HostTarget(val host: String, val port: Int) {

    /** Host with the brackets IPv6 needs in a URL authority. */
    val authority: String
        get() = if (host.contains(':')) "[$host]:$port" else "$host:$port"

    /**
     * Base URL for the Web UI. The per-launch `client` token is only needed
     * when the host gates its loopback UI; without one we open the plain root.
     */
    fun url(clientToken: String?): String {
        val base = "http://$authority/"
        val token = clientToken?.trim().orEmpty()
        return if (token.isEmpty()) base else "$base?client=${Uri.encode(token)}"
    }

    companion object {
        const val DEFAULT_PORT = 28418

        /**
         * Accepts `host`, `host:port`, `[v6]:port` and a full URL; the path,
         * query and fragment are discarded. Returns null when the text cannot
         * be a host at all, or the port is out of range.
         */
        fun parse(raw: String): HostTarget? {
            var value = raw.trim()
            if (value.isEmpty()) return null
            val scheme = value.indexOf("://")
            if (scheme >= 0) value = value.substring(scheme + 3)
            value = value.substringBefore('/')
                .substringBefore('?')
                .substringBefore('#')
            if (value.isEmpty()) return null

            val hostPart: String
            var portPart = ""
            if (value.startsWith("[")) {
                val close = value.indexOf(']')
                if (close < 0) return null
                hostPart = value.substring(1, close)
                val rest = value.substring(close + 1)
                if (rest.startsWith(":")) {
                    portPart = rest.substring(1)
                } else if (rest.isNotEmpty()) {
                    return null
                }
            } else {
                val colon = value.lastIndexOf(':')
                if (colon > 0 && value.indexOf(':') == colon) {
                    hostPart = value.substring(0, colon)
                    portPart = value.substring(colon + 1)
                } else {
                    // No port, or an unbracketed IPv6 literal.
                    hostPart = value
                }
            }

            if (hostPart.isEmpty() || hostPart.any { it.isWhitespace() }) return null
            val port = if (portPart.isEmpty()) {
                DEFAULT_PORT
            } else {
                portPart.toIntOrNull() ?: return null
            }
            if (port !in 1..65535) return null
            return HostTarget(hostPart, port)
        }

        /** True when the only problem is a non-numeric or out-of-range port. */
        fun hasInvalidPort(raw: String): Boolean {
            var value = raw.trim()
            val scheme = value.indexOf("://")
            if (scheme >= 0) value = value.substring(scheme + 3)
            value = value.substringBefore('/')
                .substringBefore('?')
                .substringBefore('#')
            if (value.isEmpty()) return false
            val portText = when {
                value.startsWith("[") ->
                    value.substringAfter(']', "").removePrefix(":")
                value.count { it == ':' } == 1 -> value.substringAfter(':')
                else -> return false
            }.trim()
            if (portText.isEmpty()) return false
            val port = portText.toIntOrNull() ?: return true
            return port !in 1..65535
        }
    }
}

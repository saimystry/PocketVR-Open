// SPDX-License-Identifier: Apache-2.0
package com.pocketvr

import java.net.Inet4Address
import java.net.NetworkInterface

/** Setup-thread policy only; discovery hints never replace authenticated pairing. */
object TransportSelection {
    data class Subnet(val address: String, val prefix: Int)
    data class Route(val ip: String, val port: Int, val kind: String)
    fun ipv4(ip: String): Long? {
        val parts = ip.split('.')
        if (parts.size != 4) return null
        var value = 0L
        for (part in parts) {
            if (part.isEmpty() || !part.all(Char::isDigit)) return null
            val octet = part.toIntOrNull() ?: return null
            if (octet !in 0..255) return null
            value = (value shl 8) or octet.toLong()
        }
        return value
    }
    fun tetherName(name: String): Boolean = Regex("^(rndis|usb|ncm)[0-9]+$").matches(name)
    fun onTether(ip: String, subnets: List<Subnet>): Boolean {
        val target = ipv4(ip) ?: return false
        return subnets.any {
            val local = ipv4(it.address)
            val mask = if (it.prefix in 1..32) (0xffffffffL shl (32 - it.prefix)) and 0xffffffffL else 0L
            local != null && mask != 0L && target != local && (target and mask) == (local and mask)
        }
    }
    fun tetherSubnets(): List<Subnet> = try {
        NetworkInterface.getNetworkInterfaces().toList().filter { it.isUp && tetherName(it.name) }
            .flatMap { nic -> nic.interfaceAddresses.mapNotNull {
                val ip = it.address as? Inet4Address
                if (ip == null) null else Subnet(ip.hostAddress ?: "", it.networkPrefixLength.toInt())
            } }.take(16)
    } catch (_: Exception) { emptyList() }

    fun routes(discovered: List<Route>, subnets: List<Subnet>, saved: Route?): List<Route> {
        fun tether(route: Route) = route.kind == "USB tethering" || onTether(route.ip, subnets)
        val tether = (discovered + listOfNotNull(saved)).filter { tether(it) }.sortedBy { it.ip }
        // Unpaired first use must select a PC explicitly when discovery is ambiguous.
        return (tether + Route("127.0.0.1", saved?.port ?: 9943, "adb USB") +
            listOfNotNull(saved?.takeIf { it.ip != "127.0.0.1" }) +
            discovered.filterNot { tether(it) }.sortedBy { it.ip })
            .distinctBy { "${it.ip}:${it.port}" }.take(18)
    }
}

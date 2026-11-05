// SPDX-License-Identifier: Apache-2.0
package com.pocketvr.test

import com.pocketvr.TransportSelection as T

object TransportSelectionChecks {
    @JvmStatic fun run() {
        check(T.ipv4("256.1.1.1") == null && T.ipv4("1.2.3") == null && T.ipv4("-1.2.3.4") == null)
        check(T.ipv4("99999999999999.1.2.3") == null)
        check(T.tetherName("rndis0") && T.tetherName("usb12") && T.tetherName("ncm0"))
        check(!T.tetherName("wlan0") && !T.tetherName("tun0") && !T.tetherName("usb_wifi"))
        val subnet = listOf(T.Subnet("192.168.42.129", 24))
        check(T.onTether("192.168.42.2", subnet))
        check(!T.onTether("192.168.43.2", subnet) && !T.onTether("192.168.42.129", subnet))
        check(!T.onTether("192.168.42.2", listOf(T.Subnet("192.168.42.129", 0))))
        check(T.onTether("172.20.10.6", listOf(T.Subnet("172.20.10.5", 30))))
        check(!T.onTether("172.20.10.8", listOf(T.Subnet("172.20.10.5", 30))))
        val wifi = T.Route("192.168.0.100", 9943, "Wi-Fi / LAN")
        val tether = T.Route("192.168.42.2", 9943, "USB tethering")
        val routes = T.routes(listOf(wifi, tether, wifi), subnet, wifi)
        check(routes.map { it.kind } == listOf("USB tethering", "adb USB", "Wi-Fi / LAN"))
        check(T.routes(emptyList(), emptyList(), wifi).map { it.ip } == listOf("127.0.0.1", wifi.ip))
        check(T.routes(emptyList(), emptyList(), null).single().kind == "adb USB")
    }
}

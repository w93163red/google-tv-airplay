package dev.airscreen

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.util.Log

class ServiceAdvertiser(context: Context) {
    private val nsd = context.getSystemService(NsdManager::class.java)
    private val registrations = HashMap<Int, NsdManager.RegistrationListener>()

    fun register(service: Int, name: String, port: Int, txt: ByteArray) {
        val attributes = parseTxt(txt)
        val joined = attributes.joinToString(";") { (k, v) -> "$k=$v" }
        val type = if (service == Native.SERVICE_RAOP) {
            Log.i(TAG, "TXT raop $name $joined")
            "_raop._tcp"
        } else {
            Log.i(TAG, "TXT airplay $joined")
            "_airplay._tcp"
        }
        val info = NsdServiceInfo().apply {
            serviceName = name
            serviceType = type
            this.port = port
            attributes.forEach { (k, v) -> setAttribute(k, v) }
        }
        val listener = object : NsdManager.RegistrationListener {
            override fun onServiceRegistered(info: NsdServiceInfo) {
                Log.i(TAG, "registered $type as ${info.serviceName}")
            }
            override fun onRegistrationFailed(info: NsdServiceInfo, error: Int) {
                Log.e(TAG, "registering $type failed: $error")
            }
            override fun onServiceUnregistered(info: NsdServiceInfo) {}
            override fun onUnregistrationFailed(info: NsdServiceInfo, error: Int) {
                Log.e(TAG, "unregistering $type failed: $error")
            }
        }
        synchronized(registrations) { registrations.put(service, listener) }
            ?.let { nsd.unregisterService(it) }
        nsd.registerService(info, NsdManager.PROTOCOL_DNS_SD, listener)
    }

    fun unregister(service: Int) {
        synchronized(registrations) { registrations.remove(service) }?.let { nsd.unregisterService(it) }
    }

    companion object {
        private const val TAG = "AirScreen"

        /** Splits DNS-SD wire-format TXT (length-prefixed "key=value" strings). */
        fun parseTxt(txt: ByteArray): List<Pair<String, String>> {
            val out = ArrayList<Pair<String, String>>()
            var i = 0
            while (i < txt.size) {
                val len = txt[i].toInt() and 0xff
                val item = String(txt, i + 1, len, Charsets.UTF_8)
                out += item.substringBefore('=') to item.substringAfter('=', "")
                i += 1 + len
            }
            return out
        }
    }
}

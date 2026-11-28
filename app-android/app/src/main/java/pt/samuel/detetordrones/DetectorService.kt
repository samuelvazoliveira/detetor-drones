package pt.samuel.detetordrones

import android.annotation.SuppressLint
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.ParcelUuid
import android.util.Log
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.util.UUID

/**
 * Serviço em primeiro plano: mantém a ligação Bluetooth ao detetor (Nordic UART Service),
 * guarda tudo o que chega, lança os alertas e, ao ligar, acerta a hora do detetor e
 * descarrega o histórico do que aconteceu enquanto o telemóvel estava longe.
 */
@SuppressLint("MissingPermission") // as permissões são pedidas no MainActivity antes de arrancar
class DetectorService : Service() {
    companion object {
        val NUS: UUID = UUID.fromString("6e400001-b5a3-f393-e0a9-e50e24dcca9e")
        val RX: UUID = UUID.fromString("6e400002-b5a3-f393-e0a9-e50e24dcca9e")
        val TX: UUID = UUID.fromString("6e400003-b5a3-f393-e0a9-e50e24dcca9e")
        val CCCD: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
        const val NAME = "DetetorDrones"
        const val CH_ALERT = "alertas"
        const val CH_SERVICE = "servico"
        const val TAG = "Detetor"
        var instance: DetectorService? = null

        /** Diz ao detetor para nunca mais avisar por este id (ex.: rede Wi-Fi de um vizinho). */
        fun ignore(ctx: Context, id: String) {
            val p = ctx.getSharedPreferences("cfg", 0)
            p.edit().putStringSet("ignored", (p.getStringSet("ignored", emptySet())!! + id)).apply()
            instance?.send(JSONObject().put("cmd", "ignore").put("id", id))
        }

        fun start(ctx: Context) {
            ctx.startForegroundService(Intent(ctx, DetectorService::class.java))
        }

        fun forget(ctx: Context) {
            ctx.getSharedPreferences("cfg", 0).edit().remove("addr").apply()
            ctx.stopService(Intent(ctx, DetectorService::class.java))
            start(ctx)
        }
    }

    private val handler = Handler(Looper.getMainLooper())
    private lateinit var prefs: SharedPreferences
    private var gatt: BluetoothGatt? = null
    private var scanning = false
    private val rx = ByteArrayOutputStream()
    private val writeQueue = ArrayDeque<ByteArray>()
    private var writing = false
    private var histNewAlerts = 0
    private var notifId = 100

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        instance = this
        Repo.init(this)
        prefs = getSharedPreferences("cfg", 0)
        createChannels()
        startForeground(1, ongoing("A procurar o detetor…"),
            ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        connectOrScan()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int) = START_STICKY

    override fun onDestroy() {
        instance = null
        stopScan()
        gatt?.close()
        gatt = null
        setConn("Desligado")
        super.onDestroy()
    }

    // ------------------------------------------------------------------ ligação
    private fun adapter() = (getSystemService(BLUETOOTH_SERVICE) as BluetoothManager).adapter

    private fun connectOrScan() {
        val a = adapter()
        if (a == null || !a.isEnabled) {
            setConn("Bluetooth desligado")
            handler.postDelayed({ connectOrScan() }, 10_000)
            return
        }
        val addr = prefs.getString("addr", null)
        if (addr != null) connect(a.getRemoteDevice(addr), auto = true) else scan()
    }

    private val scanCb = object : ScanCallback() {
        override fun onScanResult(type: Int, r: ScanResult) {
            stopScan()
            prefs.edit().putString("addr", r.device.address).apply()
            connect(r.device, auto = false)
        }
    }

    private fun scan() {
        val s = adapter().bluetoothLeScanner ?: return
        setConn("À procura do detetor…")
        val filters = listOf(
            ScanFilter.Builder().setServiceUuid(ParcelUuid(NUS)).build(),
            ScanFilter.Builder().setDeviceName(NAME).build(),
        )
        val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_BALANCED).build()
        s.startScan(filters, settings, scanCb)
        scanning = true
    }

    private fun stopScan() {
        if (scanning) adapter()?.bluetoothLeScanner?.stopScan(scanCb)
        scanning = false
    }

    private fun connect(dev: BluetoothDevice, auto: Boolean) {
        // autoConnect=true: o Android liga sozinho quando o detetor volta a estar ao alcance
        setConn(if (auto) "À espera do detetor (fora de alcance?)" else "A ligar…")
        gatt?.close()
        gatt = dev.connectGatt(this, auto, gattCb, BluetoothDevice.TRANSPORT_LE)
    }

    private val gattCb = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, state: Int) {
            handler.post {
                if (state == BluetoothProfile.STATE_CONNECTED) {
                    setConn("Ligado: a preparar…")
                    g.requestMtu(247)
                } else if (state == BluetoothProfile.STATE_DISCONNECTED) {
                    setConn("Desligado")
                    writeQueue.clear(); writing = false; rx.reset()
                    g.close()
                    if (gatt === g) gatt = null
                    handler.postDelayed({ if (gatt == null) connectOrScan() }, 3000)
                }
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            g.discoverServices()
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            val tx = g.getService(NUS)?.getCharacteristic(TX)
            if (tx == null) {
                Log.w(TAG, "serviço NUS não encontrado"); g.disconnect(); return
            }
            g.setCharacteristicNotification(tx, true)
            val d = tx.getDescriptor(CCCD) ?: return
            if (Build.VERSION.SDK_INT >= 33) {
                g.writeDescriptor(d, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
            } else {
                @Suppress("DEPRECATION")
                d.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                @Suppress("DEPRECATION")
                g.writeDescriptor(d)
            }
        }

        override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int) {
            handler.post {
                setConn("Ligado")
                send(JSONObject().put("cmd", "time").put("epoch", System.currentTimeMillis() / 1000.0))
                getSharedPreferences("cfg", 0).getStringSet("ignored", emptySet())!!.forEach {
                    send(JSONObject().put("cmd", "ignore").put("id", it))
                }
                requestHistory()
            }
        }

        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray) {
            handler.post { onBytes(value) }
        }

        @Deprecated("Android 12")
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
            if (Build.VERSION.SDK_INT < 33) {
                @Suppress("DEPRECATION")
                val v = c.value ?: return
                handler.post { onBytes(v) }
            }
        }

        override fun onCharacteristicWrite(g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) {
            handler.post { writing = false; pump() }
        }
    }

    // ------------------------------------------------------------------ envio
    fun send(o: JSONObject) {
        writeQueue.addLast((o.toString() + "\n").toByteArray())
        pump()
    }

    private fun pump() {
        if (writing || writeQueue.isEmpty()) return
        val g = gatt ?: return
        val ch = g.getService(NUS)?.getCharacteristic(RX) ?: return
        val data = writeQueue.removeFirst()
        writing = true
        if (Build.VERSION.SDK_INT >= 33) {
            g.writeCharacteristic(ch, data, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT)
        } else {
            @Suppress("DEPRECATION")
            ch.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
            @Suppress("DEPRECATION")
            ch.value = data
            @Suppress("DEPRECATION")
            g.writeCharacteristic(ch)
        }
    }

    private fun requestHistory() {
        send(JSONObject().put("cmd", "hist").put("since", prefs.getLong("last_id", 0)).put("limit", 300))
    }

    // ------------------------------------------------------------------ receção
    private fun onBytes(b: ByteArray) {
        // Junta em bytes e só descodifica linhas inteiras: um 'ã' pode vir partido em 2 pedaços.
        for (x in b) {
            if (x == '\n'.code.toByte()) {
                val line = rx.toString(Charsets.UTF_8.name())
                rx.reset()
                if (line.isNotBlank()) handleLine(line)
            } else rx.write(x.toInt())
        }
        if (rx.size() > 65536) rx.reset()
    }

    private fun handleLine(line: String) {
        val m = try { JSONObject(line) } catch (e: Exception) { Log.w(TAG, "linha inválida"); return }
        when (m.optString("t")) {
            "alert" -> {
                val e = Ev.from(m)
                if (Repo.db.insert(e)) notifyAlert(e.title, e)
                Repo.refresh()
            }
            "track" -> Repo.db.insert(Ev.from(m))
            "still" -> {
                // drone continua por perto: volta a tocar, na mesma notificação desse drone
                val e = Ev.from(m)
                val min = m.optLong("since_s") / 60
                val sec = m.optLong("since_s") % 60
                notifyAlert("Ainda presente: ${e.title}", e, "Há ${min} min ${sec} s por perto")
            }
            "hist" -> {
                val e = Ev.from(m)
                if (Repo.db.insert(e) && e.kind == "alert") histNewAlerts++
                if (e.rid > prefs.getLong("last_id", 0)) prefs.edit().putLong("last_id", e.rid).apply()
            }
            "hist_end" -> {
                if (histNewAlerts > 0) {
                    notifyAlert("$histNewAlerts alerta(s) enquanto estavas longe", null)
                    histNewAlerts = 0
                }
                Repo.refresh()
                if (m.optBoolean("more")) requestHistory()
            }
            "status" -> {
                Repo.status.value = m
                // O detetor foi reinstalado ou a base de dados apagada: recomeçar a contagem
                if (m.optLong("last_id") < prefs.getLong("last_id", 0)) {
                    prefs.edit().putLong("last_id", 0).apply()
                }
            }
            "error" -> Log.w(TAG, "detetor: ${m.optString("msg")}")
        }
    }

    // ------------------------------------------------------------------ notificações
    private fun createChannels() {
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(
            NotificationChannel(CH_ALERT, "Alertas de drones", NotificationManager.IMPORTANCE_HIGH).apply {
                enableVibration(true)
                vibrationPattern = longArrayOf(0, 400, 200, 400)
            })
        nm.createNotificationChannel(
            NotificationChannel(CH_SERVICE, "Ligação ao detetor", NotificationManager.IMPORTANCE_LOW))
    }

    private fun openApp(): PendingIntent = PendingIntent.getActivity(
        this, 0, Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
        PendingIntent.FLAG_IMMUTABLE)

    private fun ongoing(text: String): Notification =
        Notification.Builder(this, CH_SERVICE)
            .setSmallIcon(android.R.drawable.ic_menu_compass)
            .setContentTitle("Detetor de Drones")
            .setContentText(text)
            .setOngoing(true)
            .setContentIntent(openApp())
            .build()

    private fun setConn(s: String) {
        Repo.conn.value = s
        getSystemService(NotificationManager::class.java).notify(1, ongoing(s))
    }

    private fun notifyAlert(title: String, e: Ev?, extra: String? = null) {
        if (e?.droneId != null && getSharedPreferences("cfg", 0).getStringSet("ignored", emptySet())!!.contains(e.droneId)) return
        val base = when {
            e == null -> "Abre a app para ver o histórico"
            e.isRf -> "Rádio típico de drone, sem Remote ID. Não dá posição nem piloto."
            e.isWifiHint -> "Rede Wi-Fi típica de drone. Sem identificação."
            e.data.has("op_dist_m") -> "Piloto a ${e.data.optInt("op_dist_m")} m de casa"
            else -> "Remote ID recebido"
        }
        val body = if (extra != null) "$extra. $base" else base
        val n = Notification.Builder(this, CH_ALERT)
            .setSmallIcon(android.R.drawable.ic_dialog_alert)
            .setContentTitle(title)
            .setContentText(body)
            .setCategory(Notification.CATEGORY_ALARM)
            .setAutoCancel(true)
            .setContentIntent(openApp())
            .build()
        // uma notificação por drone: os lembretes substituem a anterior e voltam a tocar
        val id = e?.droneId?.let { 1000 + (it.hashCode() and 0xFFFFF) } ?: notifId++
        getSystemService(NotificationManager::class.java).notify(id, n)
    }
}

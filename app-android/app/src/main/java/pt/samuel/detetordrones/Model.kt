package pt.samuel.detetordrones

import android.content.ContentValues
import android.content.Context
import android.database.sqlite.SQLiteDatabase
import android.database.sqlite.SQLiteOpenHelper
import kotlinx.coroutines.flow.MutableStateFlow
import org.json.JSONObject

/** Um evento vindo do detetor (alerta ou ponto de trajeto). */
data class Ev(
    val rid: Long,          // id no detetor
    val ts: Double,         // hora Unix (s)
    val timeOk: Boolean,    // a hora do detetor estava certa?
    val kind: String,       // alert | track
    val source: String,     // s3 | c5 | sdr
    val droneId: String?,
    val data: JSONObject,
) {
    val isRf get() = data.has("band")
    val isWifiHint get() = data.optString("pattern") == "wifi_drone"
    val title: String get() = titleFor(this)

    companion object {
        fun from(m: JSONObject) = Ev(
            rid = m.optLong("id"),
            ts = m.optDouble("ts"),
            timeOk = m.optBoolean("time_ok", true),
            kind = m.optString("kind"),
            source = m.optString("source"),
            droneId = if (m.isNull("drone_id")) null else m.optString("drone_id"),
            data = m.optJSONObject("data") ?: JSONObject(),
        )
    }
}

fun titleFor(e: Ev): String {
    val d = e.data
    if (e.isWifiHint) {
        val name = d.optString("ssid").ifEmpty { d.optString("maker").ifEmpty { d.optString("mac") } }
        return "Provável drone (Wi-Fi): $name"
    }
    if (e.isRf) {
        val what = if (d.optString("pattern") == "hopping") "comando FPV" else "vídeo FPV"
        return "Rádio: $what em ${d.optString("band")}"
    }
    val parts = mutableListOf("Drone ${e.droneId ?: "?"}")
    d.optString("eu_class_txt").takeIf { it.isNotEmpty() && it != "null" }?.let { parts += it }
    if (d.has("h")) parts += "%.0f m".format(d.optDouble("h"))
    if (d.has("dist_m")) parts += "a ${d.optInt("dist_m")} m de casa"
    return parts.joinToString(" · ")
}

class EventDb(ctx: Context) : SQLiteOpenHelper(ctx, "eventos.db", null, 1) {
    override fun onCreate(db: SQLiteDatabase) {
        db.execSQL(
            "CREATE TABLE ev (rid INTEGER, ts REAL, time_ok INTEGER, kind TEXT, source TEXT, " +
                "drone_id TEXT, data TEXT, UNIQUE(rid, ts))"
        )
        db.execSQL("CREATE INDEX ev_kind_ts ON ev(kind, ts)")
    }

    override fun onUpgrade(db: SQLiteDatabase, o: Int, n: Int) {}

    /** Devolve true se o evento era novo. */
    fun insert(e: Ev): Boolean {
        val v = ContentValues().apply {
            put("rid", e.rid); put("ts", e.ts); put("time_ok", if (e.timeOk) 1 else 0)
            put("kind", e.kind); put("source", e.source); put("drone_id", e.droneId)
            put("data", e.data.toString())
        }
        return writableDatabase.insertWithOnConflict("ev", null, v, SQLiteDatabase.CONFLICT_IGNORE) != -1L
    }

    private fun query(where: String, args: Array<String>, order: String, limit: Int): List<Ev> {
        val out = mutableListOf<Ev>()
        readableDatabase.query(
            "ev", arrayOf("rid", "ts", "time_ok", "kind", "source", "drone_id", "data"),
            where, args, null, null, order, limit.toString()
        ).use { c ->
            while (c.moveToNext()) out += Ev(
                c.getLong(0), c.getDouble(1), c.getInt(2) == 1, c.getString(3), c.getString(4),
                if (c.isNull(5)) null else c.getString(5), JSONObject(c.getString(6))
            )
        }
        return out
    }

    fun alerts(limit: Int = 500) = query("kind='alert'", arrayOf(), "ts DESC", limit)
    fun alertsAsc(limit: Int = 5000) = query("kind='alert'", arrayOf(), "ts ASC", limit)
    fun track(droneId: String, from: Double, to: Double) =
        query("kind='track' AND drone_id=? AND ts BETWEEN ? AND ?",
            arrayOf(droneId, from.toString(), to.toString()), "ts ASC", 5000)
}

/** Estado partilhado entre o serviço e o ecrã. */
object Repo {
    val conn = MutableStateFlow("Desligado")
    val status = MutableStateFlow<JSONObject?>(null)
    val alerts = MutableStateFlow<List<Ev>>(emptyList())
    lateinit var db: EventDb
        private set

    fun init(ctx: Context) {
        if (!::db.isInitialized) {
            db = EventDb(ctx.applicationContext)
            refresh()
        }
    }

    fun refresh() {
        alerts.value = db.alerts()
    }
}

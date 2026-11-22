package pt.samuel.detetordrones

import android.content.Context
import android.content.Intent
import android.graphics.Paint
import android.graphics.Typeface
import android.graphics.pdf.PdfDocument
import androidx.core.content.FileProvider
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/** Gera um relatório PDF com todos os alertas, pronto a anexar a uma queixa, e abre a partilha. */
object Report {
    private val fmt = SimpleDateFormat("dd/MM/yyyy HH:mm:ss", Locale("pt", "PT"))

    fun when_(e: Ev): String = fmt.format(Date((e.ts * 1000).toLong())) +
        if (e.timeOk) "" else " (hora do detetor por confirmar)"

    fun mapsLink(lat: String, lon: String) = "https://maps.google.com/?q=$lat,$lon"

    fun lines(e: Ev): List<String> {
        val d = e.data
        val out = mutableListOf<String>()
        out += "Data/hora: ${when_(e)}"
        if (e.isWifiHint) {
            out += "Tipo: provável drone com ligação Wi-Fi (sem identificação)"
            if (d.has("ssid")) out += "Nome da rede: ${d.optString("ssid")}"
            if (d.has("maker")) out += "Fabricante (pelo endereço): ${d.optString("maker")}"
            out += "Endereço: ${d.optString("mac")} · canal ${d.optString("ch")} · sinal ${d.optString("rssi")} dBm"
            return out
        }
        if (e.isRf) {
            out += "Tipo: atividade de rádio (sem identificação)"
            out += "Banda: ${d.optString("band")} · padrão: " +
                if (d.optString("pattern") == "hopping") "saltos de frequência (comando FPV)" else "portadora larga (vídeo FPV)"
            out += "Pico: ${d.optString("peak_mhz")} MHz, ${d.optString("peak_db")} dB acima do ruído"
            return out
        }
        out += "Identificação (Remote ID): ${e.droneId}"
        d.optString("eu_class_txt").takeIf { it.isNotEmpty() && it != "null" }?.let { out += "Classe UE: $it" }
        if (d.has("op_id")) out += "ID do operador: ${d.optString("op_id")}"
        if (d.has("ua_type_txt")) out += "Tipo de aeronave: ${d.optString("ua_type_txt")}"
        if (d.has("lat")) out += "Posição do drone: ${d.optString("lat")}, ${d.optString("lon")}  ${mapsLink(d.optString("lat"), d.optString("lon"))}"
        if (d.has("h")) out += "Altura acima da descolagem: %.0f m".format(d.optDouble("h"))
        if (d.has("alt")) out += "Altitude (GPS): %.0f m".format(d.optDouble("alt"))
        if (d.has("spd")) out += "Velocidade: %.1f m/s".format(d.optDouble("spd"))
        if (d.has("dist_m")) out += "Distância do drone à casa: ${d.optInt("dist_m")} m"
        if (d.has("op_lat")) out += "Posição do piloto: ${d.optString("op_lat")}, ${d.optString("op_lon")}  ${mapsLink(d.optString("op_lat"), d.optString("op_lon"))}"
        if (d.has("op_dist_m")) out += "Distância do piloto à casa: ${d.optInt("op_dist_m")} m"
        out += "Recebido por: ${d.optString("src")} (${e.source}), sinal ${d.optString("rssi")} dBm"
        val track = e.droneId?.let { Repo.db.track(it, e.ts, e.ts + 600) }.orEmpty()
        if (track.isNotEmpty()) {
            val hs = track.mapNotNull { if (it.data.has("h")) it.data.optDouble("h") else null }
            val ds = track.mapNotNull { if (it.data.has("dist_m")) it.data.optInt("dist_m") else null }
            out += "Trajeto: ${track.size} posições até ${fmt.format(Date((track.last().ts * 1000).toLong()))}" +
                (hs.maxOrNull()?.let { " · altura máx. %.0f m".format(it) } ?: "") +
                (ds.minOrNull()?.let { " · mais perto de casa: $it m" } ?: "")
        }
        return out
    }

    fun share(ctx: Context) {
        val alerts = Repo.db.alertsAsc()
        val doc = PdfDocument()
        val W = 595; val H = 842; val M = 40
        val title = Paint().apply { textSize = 16f; typeface = Typeface.DEFAULT_BOLD; isAntiAlias = true }
        val head = Paint().apply { textSize = 11f; typeface = Typeface.DEFAULT_BOLD; isAntiAlias = true }
        val body = Paint().apply { textSize = 8.5f; isAntiAlias = true }
        var pageNo = 0
        var page: PdfDocument.Page? = null
        var y = 0f

        fun newPage() {
            page?.let { doc.finishPage(it) }
            pageNo++
            page = doc.startPage(PdfDocument.PageInfo.Builder(W, H, pageNo).create())
            y = M.toFloat()
            page!!.canvas.drawText("Detetor de Drones · relatório · página $pageNo", M.toFloat(), (H - 20).toFloat(), body)
        }

        fun line(text: String, p: Paint, gap: Float = 4f) {
            // quebra simples por largura
            var rest = text
            while (rest.isNotEmpty()) {
                val n = p.breakText(rest, true, (W - 2 * M).toFloat(), null).coerceAtLeast(1)
                if (y + p.textSize > H - 40) newPage()
                y += p.textSize
                page!!.canvas.drawText(rest.substring(0, n), M.toFloat(), y, p)
                rest = rest.substring(n)
                y += 2f
            }
            y += gap
        }

        newPage()
        line("Relatório de sobrevoos de drones", title, 6f)
        line("Gerado em ${fmt.format(Date())} · ${alerts.size} alerta(s)", body)
        Repo.status.value?.optJSONArray("home")?.let { h ->
            if (!h.isNull(0)) line("Local monitorizado: ${h.optDouble(0)}, ${h.optDouble(1)}", body)
        }
        line("Sistema passivo: só recebe as emissões Remote ID (Reg. (UE) 2019/945) e mede atividade de rádio. " +
            "Nunca transmite para os drones nem interfere com eles.", body, 10f)

        alerts.forEachIndexed { i, e ->
            line("${i + 1}. ${e.title}", head, 2f)
            lines(e).forEach { line(it, body, 0f) }
            y += 8f
        }
        page?.let { doc.finishPage(it) }

        val dir = File(ctx.cacheDir, "relatorios").apply { mkdirs() }
        val f = File(dir, "relatorio-drones-${SimpleDateFormat("yyyyMMdd-HHmm", Locale.ROOT).format(Date())}.pdf")
        f.outputStream().use { doc.writeTo(it) }
        doc.close()
        val uri = FileProvider.getUriForFile(ctx, "pt.samuel.detetordrones.files", f)
        ctx.startActivity(Intent.createChooser(
            Intent(Intent.ACTION_SEND).setType("application/pdf")
                .putExtra(Intent.EXTRA_STREAM, uri)
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION),
            "Partilhar relatório"))
    }
}

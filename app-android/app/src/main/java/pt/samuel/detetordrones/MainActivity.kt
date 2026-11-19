package pt.samuel.detetordrones

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Bundle
import android.provider.Settings
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.json.JSONObject

class MainActivity : ComponentActivity() {
    private val perms = arrayOf(
        Manifest.permission.BLUETOOTH_SCAN,
        Manifest.permission.BLUETOOTH_CONNECT,
        Manifest.permission.POST_NOTIFICATIONS,
    )

    private val ask = registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
        if (hasBt()) DetectorService.start(this)
    }

    private fun hasBt() = listOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        .all { checkSelfPermission(it) == PackageManager.PERMISSION_GRANTED }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        Repo.init(this)
        if (hasBt()) DetectorService.start(this) else ask.launch(perms)
        setContent {
            MaterialTheme(colorScheme = darkColorScheme()) {
                Surface(Modifier.fillMaxSize()) { Screen() }
            }
        }
    }

    @Composable
    private fun Screen() {
        val conn by Repo.conn.collectAsState()
        val status by Repo.status.collectAsState()
        val alerts by Repo.alerts.collectAsState()
        var open by remember { mutableStateOf<Ev?>(null) }

        Column(Modifier.fillMaxSize().padding(16.dp)) {
            Text("Detetor de Drones", fontSize = 22.sp, fontWeight = FontWeight.Bold)
            Spacer(Modifier.height(8.dp))
            StatusCard(conn, status)
            Spacer(Modifier.height(8.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(onClick = { Report.share(this@MainActivity) }) { Text("Relatório PDF") }
                OutlinedButton(onClick = {
                    startActivity(Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS))
                }) { Text("Bateria") }
                OutlinedButton(onClick = { DetectorService.forget(this@MainActivity) }) { Text("Procurar") }
            }
            Spacer(Modifier.height(12.dp))
            Text("Alertas (${alerts.size})", fontWeight = FontWeight.Bold)
            if (alerts.isEmpty()) Text("Ainda nada. Céu limpo… ou drones discretos.", color = Color.Gray)
            LazyColumn(Modifier.fillMaxSize()) {
                items(alerts, key = { "${it.rid}-${it.ts}" }) { e ->
                    Column(Modifier.fillMaxWidth().clickable { open = e }.padding(vertical = 8.dp)) {
                        Text(e.title, fontWeight = FontWeight.SemiBold,
                            color = if (e.isRf) Color(0xFFFFB74D) else Color(0xFFEF9A9A))
                        Text(Report.when_(e), fontSize = 12.sp, color = Color.Gray)
                    }
                    HorizontalDivider()
                }
            }
        }
        open?.let { e -> Detail(e) { open = null } }
    }

    @Composable
    private fun StatusCard(conn: String, st: JSONObject?) {
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(12.dp)) {
                Text(conn, fontWeight = FontWeight.SemiBold)
                if (st != null) {
                    val devs = st.optJSONObject("devs") ?: JSONObject()
                    fun ok(k: String) = devs.optJSONObject(k)?.optBoolean("ok") == true
                    val dot = { b: Boolean -> if (b) "●" else "○" }
                    Text("${dot(ok("s3"))} 2,4 GHz   ${dot(ok("c5"))} 5 GHz   ${dot(ok("sdr"))} Rádio 433 MHz–1,3 GHz",
                        fontSize = 13.sp)
                    Text(if (st.optBoolean("time_ok")) "Hora do detetor: certa"
                         else "Hora do detetor: POR CONFIRMAR (sem relógio nem telemóvel)",
                        fontSize = 13.sp,
                        color = if (st.optBoolean("time_ok")) Color.Unspecified else Color(0xFFFFB74D))
                }
            }
        }
    }

    @Composable
    private fun Detail(e: Ev, onClose: () -> Unit) {
        val d = e.data
        AlertDialog(
            onDismissRequest = onClose,
            title = { Text(e.title, fontSize = 16.sp) },
            text = {
                Column(Modifier.verticalScroll(rememberScrollState())) {
                    Report.lines(e).forEach { Text(it, fontSize = 13.sp); Spacer(Modifier.height(4.dp)) }
                }
            },
            confirmButton = {
                Row {
                    if (d.has("lat")) TextButton(onClick = { map(d.optString("lat"), d.optString("lon"), "Drone") }) { Text("Mapa drone") }
                    if (d.has("op_lat")) TextButton(onClick = { map(d.optString("op_lat"), d.optString("op_lon"), "Piloto") }) { Text("Mapa piloto") }
                }
            },
            dismissButton = { TextButton(onClick = onClose) { Text("Fechar") } },
        )
    }

    private fun map(lat: String, lon: String, label: String) {
        startActivity(Intent(Intent.ACTION_VIEW, Uri.parse("geo:$lat,$lon?q=$lat,$lon($label)")))
    }
}

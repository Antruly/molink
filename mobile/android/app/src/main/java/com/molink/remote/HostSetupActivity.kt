package com.molink.remote

import android.content.Intent
import android.os.Bundle
import android.view.View
import android.view.inputmethod.EditorInfo
import android.widget.Button
import android.widget.EditText
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity

/**
 * Launcher screen: one address field, one verification-code field. The code
 * is passed to [MainActivity] and filled into the host's login form there.
 *
 * A per-launch client token (only relevant when the host gates its loopback
 * UI) can be supplied to this activity with an intent extra; the setup UI has
 * no field for it because a remote phone normally connects without one.
 */
class HostSetupActivity : AppCompatActivity() {

    private lateinit var addressField: EditText
    private lateinit var codeField: EditText
    private lateinit var errorText: TextView
    private var clientToken: String? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_host_setup)

        addressField = findViewById(R.id.input_address)
        codeField = findViewById(R.id.input_code)
        errorText = findViewById(R.id.text_error)

        val prefs = getSharedPreferences(PREFS, MODE_PRIVATE)
        addressField.setText(initialAddress(prefs.getString(KEY_ADDRESS, null)))
        codeField.setText(intent.getStringExtra(MainActivity.EXTRA_CODE).orEmpty())
        clientToken = intent.getStringExtra(MainActivity.EXTRA_CLIENT_TOKEN)
            ?.trim()
            ?.takeIf { it.isNotEmpty() }

        findViewById<Button>(R.id.button_connect).setOnClickListener { connect() }
        addressField.setOnEditorActionListener { _, actionId, _ ->
            if (actionId == EditorInfo.IME_ACTION_NEXT) {
                codeField.requestFocus()
                true
            } else {
                false
            }
        }
        codeField.setOnEditorActionListener { _, actionId, _ ->
            if (actionId == EditorInfo.IME_ACTION_GO) {
                connect()
                true
            } else {
                false
            }
        }
        addressField.setOnFocusChangeListener { _, focused ->
            if (focused) addressField.error = null
        }
        codeField.setOnFocusChangeListener { _, focused ->
            if (focused) errorText.visibility = View.GONE
        }
    }

    private fun initialAddress(saved: String?): String {
        val host = intent.getStringExtra(MainActivity.EXTRA_HOST)
        val port = intent.getIntExtra(MainActivity.EXTRA_PORT, 0)
        if (!host.isNullOrBlank() && port in 1..65535) {
            return if (host.contains(':')) "[$host]:$port" else "$host:$port"
        }
        return saved.orEmpty()
    }

    private fun connect() {
        val raw = addressField.text.toString()
        val target = HostTarget.parse(raw)
        if (target == null) {
            if (HostTarget.hasInvalidPort(raw)) {
                errorText.text = getString(R.string.setup_error_port)
                errorText.visibility = View.VISIBLE
            } else {
                addressField.error = getString(R.string.setup_error_address)
            }
            return
        }
        errorText.visibility = View.GONE
        addressField.error = null

        getSharedPreferences(PREFS, MODE_PRIVATE)
            .edit()
            .putString(KEY_ADDRESS, target.authority)
            .apply()

        val intent = Intent(this, MainActivity::class.java).apply {
            putExtra(MainActivity.EXTRA_HOST, target.host)
            putExtra(MainActivity.EXTRA_PORT, target.port)
            putExtra(MainActivity.EXTRA_CODE, codeField.text.toString().trim())
            clientToken?.let { putExtra(MainActivity.EXTRA_CLIENT_TOKEN, it) }
        }
        startActivity(intent)
    }

    companion object {
        private const val PREFS = "molink.android"
        private const val KEY_ADDRESS = "last_address"
    }
}

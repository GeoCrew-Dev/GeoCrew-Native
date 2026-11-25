package org.geocrew.nativelib.sample

import android.app.Activity
import android.os.Bundle
import android.widget.TextView
import org.geocrew.nativelib.GeoCrewNative

/** Shows the library versions, proving the native libraries load. */
class MainActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(TextView(this).apply { text = GeoCrewNative.versions() })
    }
}

package com.senter.shotsync

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class TestShotSync {

    @Test
    fun testFunctionPointer() {
        for (i in 1..20) {
            val handler = ShotSync.rpc_handler_t()
            println("ClientSetMessageHandler No. $i $handler")
            ShotSync.ClientSetMessageHandler(handler)
        }
    }
}
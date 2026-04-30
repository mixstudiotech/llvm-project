package org.llvm.dtx

import java.net.InetAddress
import java.net.Socket

interface Transport : AutoCloseable {
    fun read(buffer: ByteArray, offset: Int, length: Int): Int
    fun write(bytes: ByteArray)
}

class TcpTransport private constructor(private val socket: Socket) : Transport {
    private val input = socket.getInputStream()
    private val output = socket.getOutputStream()

    override fun read(buffer: ByteArray, offset: Int, length: Int): Int =
        input.read(buffer, offset, length)

    override fun write(bytes: ByteArray) {
        output.write(bytes)
        output.flush()
    }

    override fun close() {
        socket.close()
    }

    companion object {
        fun connectLoopback(port: Int): TcpTransport {
            val socket = Socket(InetAddress.getLoopbackAddress(), port)
            socket.tcpNoDelay = true
            return TcpTransport(socket)
        }
    }
}

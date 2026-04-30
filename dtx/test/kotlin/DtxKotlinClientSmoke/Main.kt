import java.util.concurrent.TimeUnit
import org.llvm.dtx.Connection
import org.llvm.dtx.DtxException
import org.llvm.dtx.NSObject
import org.llvm.dtx.TcpTransport
import org.llvm.dtx.debughost.InitializeRequest
import org.llvm.dtx.debughost.LifecycleClient

fun main(args: Array<String>) {
    if (args.size != 1) {
        System.err.println("usage: DtxKotlinClientSmoke <dtx-debughost-tcp-server>")
        kotlin.system.exitProcess(2)
    }

    val server = ProcessBuilder(args[0])
        .redirectError(ProcessBuilder.Redirect.PIPE)
        .start()

    try {
        val line = server.inputStream.bufferedReader().readLine()
        if (line == null || !line.startsWith("PORT="))
            throw DtxException("C++ DebugHost server did not print a port.")

        val port = line.removePrefix("PORT=").toInt()
        TcpTransport.connectLoopback(port).use { transport ->
            Connection(transport).use { connection ->
                val client = LifecycleClient(connection)
                val request = InitializeRequest(NSObject.string("kotlin-request"))
                val reply = client.initialize(request)
                if (reply.raw != NSObject.string("kotlin-initialized"))
                    throw DtxException("Unexpected initialize reply: ${reply.raw}")
            }
        }

        if (!server.waitFor(15, TimeUnit.SECONDS))
            throw DtxException("C++ DebugHost server did not exit.")
        if (server.exitValue() != 0)
            throw DtxException("C++ DebugHost server exited with ${server.exitValue()}: ${server.errorStream.bufferedReader().readText()}")
    } catch (ex: Throwable) {
        if (server.isAlive)
            server.destroyForcibly()
        System.err.println(ex)
        System.err.println(server.errorStream.bufferedReader().readText())
        kotlin.system.exitProcess(1)
    }
}

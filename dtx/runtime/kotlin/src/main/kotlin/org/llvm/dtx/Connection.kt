package org.llvm.dtx

class Connection(private val transport: Transport) : AutoCloseable {
    private var nextMessageId = 0

    fun call(channel: Int, selector: String, args: AuxList, expectsReply: Boolean = true): NSObject {
        val payload = Fragment.buildSelectorPayload(selector, args)
        val messageId = ++nextMessageId
        val header = MessageHeader.build(channel, payload.size, messageId, 0, expectsReply)
        transport.write(Fragment.buildFrame(header, payload))

        if (!expectsReply)
            return NSObject.Null

        val replyHeaderBytes = readExact(32)
        val replyHeader = MessageHeader.decode(replyHeaderBytes)
        val replyPayload = readExact(replyHeader.payloadLength)
        return Fragment.parseObjectPayload(replyPayload)
    }

    private fun readExact(size: Int): ByteArray {
        val bytes = ByteArray(size)
        var offset = 0
        while (offset < size) {
            val count = transport.read(bytes, offset, size - offset)
            if (count == -1)
                throw DtxException("Transport reached EOF.")
            offset += count
        }
        return bytes
    }

    override fun close() {
        transport.close()
    }
}

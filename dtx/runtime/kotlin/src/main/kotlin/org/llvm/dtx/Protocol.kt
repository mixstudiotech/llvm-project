package org.llvm.dtx

enum class PayloadFlag(val value: Int) {
    Empty(0x00),
    AsyncObject(0x01),
    Selector(0x02),
    Reply(0x03),
    Object(0x04),
    Null(0x05),
}

data class MessageHeader(
    val magic: Int,
    val headerLength: Int,
    val fragmentId: Int,
    val fragmentCount: Int,
    val payloadLength: Int,
    val messageId: Int,
    val conversationIndex: Int,
    val channelCode: Int,
    val expectsReply: Int,
) {
    fun encode(): ByteArray {
        val bytes = ByteArray(32)
        putU32(bytes, 0, magic)
        putU32(bytes, 4, headerLength)
        putU16(bytes, 8, fragmentId)
        putU16(bytes, 10, fragmentCount)
        putU32(bytes, 12, payloadLength)
        putU32(bytes, 16, messageId)
        putU32(bytes, 20, conversationIndex)
        putU32(bytes, 24, channelCode)
        putU32(bytes, 28, expectsReply)
        return bytes
    }

    companion object {
        const val EXPECTED_MAGIC = 0x1F3D5B79
        const val EXPECTED_HEADER_LENGTH = 0x20

        fun build(channelCode: Int, payloadLength: Int, messageId: Int, conversationIndex: Int, expectsReply: Boolean): MessageHeader =
            MessageHeader(EXPECTED_MAGIC, EXPECTED_HEADER_LENGTH, 0, 1, payloadLength, messageId, conversationIndex, channelCode, if (expectsReply) 1 else 0)

        fun decode(bytes: ByteArray): MessageHeader {
            if (bytes.size < 32)
                throw DtxException("DTX message header is shorter than 32 bytes.")
            val header = MessageHeader(
                u32At(bytes, 0),
                u32At(bytes, 4),
                u16At(bytes, 8),
                u16At(bytes, 10),
                u32At(bytes, 12),
                u32At(bytes, 16),
                u32At(bytes, 20),
                u32At(bytes, 24),
                u32At(bytes, 28),
            )
            if (header.magic != EXPECTED_MAGIC)
                throw DtxException("Bad DTX message magic.")
            if (header.headerLength != EXPECTED_HEADER_LENGTH)
                throw DtxException("Bad DTX message header length.")
            return header
        }
    }
}

data class PayloadHeader(val flags: Int, val auxiliaryLength: Int, val totalLength: Long) {
    fun encode(): ByteArray {
        val bytes = ByteArray(16)
        putU32(bytes, 0, flags)
        putU32(bytes, 4, auxiliaryLength)
        putU64(bytes, 8, totalLength)
        return bytes
    }

    companion object {
        fun decode(bytes: ByteArray, offset: Int = 0): PayloadHeader {
            if (bytes.size - offset < 16)
                throw DtxException("DTX payload header is shorter than 16 bytes.")
            return PayloadHeader(u32At(bytes, offset), u32At(bytes, offset + 4), u64At(bytes, offset + 8))
        }
    }
}

private fun u16At(bytes: ByteArray, offset: Int): Int =
    (bytes[offset].toInt() and 0xff) or ((bytes[offset + 1].toInt() and 0xff) shl 8)

private fun putU16(bytes: ByteArray, offset: Int, value: Int) {
    bytes[offset] = (value and 0xff).toByte()
    bytes[offset + 1] = ((value ushr 8) and 0xff).toByte()
}

private fun putU32(bytes: ByteArray, offset: Int, value: Int) {
    bytes[offset] = (value and 0xff).toByte()
    bytes[offset + 1] = ((value ushr 8) and 0xff).toByte()
    bytes[offset + 2] = ((value ushr 16) and 0xff).toByte()
    bytes[offset + 3] = ((value ushr 24) and 0xff).toByte()
}

private fun putU64(bytes: ByteArray, offset: Int, value: Long) {
    var current = value
    repeat(8) { index ->
        bytes[offset + index] = (current and 0xffL).toByte()
        current = current ushr 8
    }
}

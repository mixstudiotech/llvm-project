package org.llvm.dtx

import java.io.ByteArrayOutputStream

object Fragment {
    fun buildSelectorPayload(selector: String, args: AuxList): ByteArray {
        val auxBytes = args.encode()
        val selectorBytes = KeyedArchiver.archiveRoot(NSObject.string(selector))
        return buildPayload(PayloadFlag.Selector, auxBytes, selectorBytes)
    }

    fun buildObjectPayload(value: NSObject, flag: PayloadFlag = PayloadFlag.Reply): ByteArray =
        buildPayload(flag, ByteArray(0), KeyedArchiver.archiveRoot(value))

    fun buildFrame(header: MessageHeader, payload: ByteArray): ByteArray {
        val out = ByteArrayOutputStream()
        out.write(header.encode())
        out.write(payload)
        return out.toByteArray()
    }

    fun parseObjectPayload(payload: ByteArray): NSObject {
        val header = PayloadHeader.decode(payload)
        if (header.flags != PayloadFlag.Reply.value &&
            header.flags != PayloadFlag.Object.value &&
            header.flags != PayloadFlag.AsyncObject.value)
            throw DtxException("Payload is not an object reply.")
        if (header.auxiliaryLength != 0)
            throw DtxException("Object payload unexpectedly contains aux bytes.")
        if (16 + header.totalLength.toInt() != payload.size)
            throw DtxException("Object payload length mismatch.")
        if (header.totalLength == 0L)
            return NSObject.Null
        return KeyedArchiver.unarchiveRoot(payload, 16, header.totalLength.toInt())
    }

    private fun buildPayload(flag: PayloadFlag, auxBytes: ByteArray, bodyBytes: ByteArray): ByteArray {
        val header = PayloadHeader(flag.value, auxBytes.size, (auxBytes.size + bodyBytes.size).toLong())
        val out = ByteArrayOutputStream()
        out.write(header.encode())
        out.write(auxBytes)
        out.write(bodyBytes)
        return out.toByteArray()
    }
}

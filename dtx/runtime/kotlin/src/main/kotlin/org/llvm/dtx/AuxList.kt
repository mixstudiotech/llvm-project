package org.llvm.dtx

import java.io.ByteArrayOutputStream

sealed class Arg {
    data object NullArg : Arg()
    data class I32Arg(val value: Int) : Arg()
    data class I64Arg(val value: Long) : Arg()
    data class U32Arg(val value: Int) : Arg()
    data class U64Arg(val value: Long) : Arg()
    data class ObjectArg(val value: NSObject) : Arg()

    companion object {
        fun nullArg(): Arg = NullArg
        fun i32(value: Int): Arg = I32Arg(value)
        fun i64(value: Long): Arg = I64Arg(value)
        fun u32(value: Int): Arg = U32Arg(value)
        fun u64(value: Long): Arg = U64Arg(value)
        fun obj(value: NSObject): Arg = ObjectArg(value)
    }
}

class AuxList {
    private val values = ArrayList<Arg>()
    val args: List<Arg>
        get() = values

    fun add(arg: Arg) {
        values.add(arg)
    }

    fun encode(): ByteArray {
        val body = ByteArrayOutputStream()
        values.forEach { arg ->
            when (arg) {
                Arg.NullArg -> {
                    writeType(body, 2)
                    writeU32(body, 0)
                }
                is Arg.I32Arg -> {
                    writeType(body, 3)
                    writeU32(body, arg.value)
                }
                is Arg.I64Arg -> {
                    writeType(body, 4)
                    writeU64(body, arg.value)
                }
                is Arg.U32Arg -> {
                    writeType(body, 5)
                    writeU32(body, arg.value)
                }
                is Arg.U64Arg -> {
                    writeType(body, 6)
                    writeU64(body, arg.value)
                }
                is Arg.ObjectArg -> {
                    writeType(body, 2)
                    val bytes = KeyedArchiver.archiveRoot(arg.value)
                    writeU32(body, bytes.size)
                    body.write(bytes)
                }
            }
        }

        val bodyBytes = body.toByteArray()
        val out = ByteArrayOutputStream()
        writeU64(out, AUX_MAGIC)
        writeU64(out, bodyBytes.size.toLong())
        out.write(bodyBytes)
        return out.toByteArray()
    }

    companion object {
        private const val AUX_MAGIC = 0x01F0L

        fun decode(bytes: ByteArray): AuxList {
            if (bytes.size < 16)
                throw DtxException("Aux list is shorter than header.")
            if (u64At(bytes, 0) != AUX_MAGIC)
                throw DtxException("Bad aux magic.")
            val bodyLength = u64At(bytes, 8)
            if (bodyLength.toInt() + 16 != bytes.size)
                throw DtxException("Aux length does not match buffer size.")

            val list = AuxList()
            var offset = 16
            while (offset < bytes.size) {
                val typeTag = u32At(bytes, offset)
                offset += 4
                val type = u32At(bytes, offset)
                offset += 4
                if (typeTag != 10)
                    throw DtxException("Bad aux type tag.")
                when (type) {
                    2 -> {
                        val objectLength = u32At(bytes, offset)
                        offset += 4
                        if (objectLength == 0) {
                            list.add(Arg.nullArg())
                        } else {
                            if (offset + objectLength > bytes.size)
                                throw DtxException("Object aux payload extends past buffer.")
                            list.add(Arg.obj(KeyedArchiver.unarchiveRoot(bytes, offset, objectLength)))
                            offset += objectLength
                        }
                    }
                    3 -> {
                        list.add(Arg.i32(u32At(bytes, offset)))
                        offset += 4
                    }
                    4 -> {
                        list.add(Arg.i64(u64At(bytes, offset)))
                        offset += 8
                    }
                    5 -> {
                        list.add(Arg.u32(u32At(bytes, offset)))
                        offset += 4
                    }
                    6 -> {
                        list.add(Arg.u64(u64At(bytes, offset)))
                        offset += 8
                    }
                    else -> throw DtxException("Unsupported aux type.")
                }
            }
            return list
        }

        private fun writeType(out: ByteArrayOutputStream, type: Int) {
            writeU32(out, 10)
            writeU32(out, type)
        }
    }
}

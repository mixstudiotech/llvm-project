package org.llvm.dtx

import java.io.ByteArrayOutputStream
import java.nio.charset.StandardCharsets

object KeyedArchiver {
    private val magic = byteArrayOf('D'.code.toByte(), 'T'.code.toByte(), 'X'.code.toByte(), 'K'.code.toByte(), 'A'.code.toByte(), 'R'.code.toByte(), '1'.code.toByte(), 0)

    private object Tag {
        const val NULL = 0
        const val BOOL = 1
        const val INT = 2
        const val UINT = 3
        const val DOUBLE = 4
        const val STRING = 5
        const val DATA = 6
        const val ARRAY = 7
        const val DICT = 8
    }

    fun archiveRoot(root: NSObject): ByteArray {
        val out = ByteArrayOutputStream()
        out.write(magic)
        writeObject(out, root)
        return out.toByteArray()
    }

    fun unarchiveRoot(bytes: ByteArray): NSObject {
        if (bytes.size < magic.size || !bytes.copyOfRange(0, magic.size).contentEquals(magic))
            throw DtxException("Bad object archive magic.")
        val reader = Reader(bytes, magic.size)
        val root = reader.readObject()
        if (!reader.consumed)
            throw DtxException("Trailing bytes after object archive root.")
        return root
    }

    fun unarchiveRoot(bytes: ByteArray, offset: Int, length: Int): NSObject =
        unarchiveRoot(bytes.copyOfRange(offset, offset + length))

    private fun writeObject(out: ByteArrayOutputStream, value: NSObject) {
        when (value) {
            NSObject.Null -> out.write(Tag.NULL)
            is NSObject.BoolValue -> {
                out.write(Tag.BOOL)
                out.write(if (value.value) 1 else 0)
            }
            is NSObject.IntValue -> {
                out.write(Tag.INT)
                writeU64(out, value.value)
            }
            is NSObject.UIntValue -> {
                out.write(Tag.UINT)
                writeU64(out, value.value)
            }
            is NSObject.DoubleValue -> {
                out.write(Tag.DOUBLE)
                writeU64(out, java.lang.Double.doubleToRawLongBits(value.value))
            }
            is NSObject.StringValue -> {
                out.write(Tag.STRING)
                writeBytes(out, value.value.toByteArray(StandardCharsets.UTF_8))
            }
            is NSObject.DataValue -> {
                out.write(Tag.DATA)
                writeBytes(out, value.value)
            }
            is NSObject.ArrayValue -> {
                out.write(Tag.ARRAY)
                writeU32(out, value.value.size)
                value.value.forEach { writeObject(out, it) }
            }
            is NSObject.DictValue -> {
                out.write(Tag.DICT)
                val sorted = value.value.toSortedMap()
                writeU32(out, sorted.size)
                sorted.forEach { (key, item) ->
                    writeBytes(out, key.toByteArray(StandardCharsets.UTF_8))
                    writeObject(out, item)
                }
            }
        }
    }

    private fun writeBytes(out: ByteArrayOutputStream, bytes: ByteArray) {
        writeU32(out, bytes.size)
        out.write(bytes)
    }

    private class Reader(private val bytes: ByteArray, private var offset: Int = 0) {
        val consumed: Boolean
            get() = offset == bytes.size

        fun readObject(): NSObject =
            when (val tag = readByte().toInt() and 0xff) {
                Tag.NULL -> NSObject.Null
                Tag.BOOL -> NSObject.bool(readByte().toInt() != 0)
                Tag.INT -> NSObject.int(readU64())
                Tag.UINT -> NSObject.uint(readU64())
                Tag.DOUBLE -> NSObject.double(java.lang.Double.longBitsToDouble(readU64()))
                Tag.STRING -> NSObject.string(String(readBytes(), StandardCharsets.UTF_8))
                Tag.DATA -> NSObject.data(readBytes())
                Tag.ARRAY -> readArray()
                Tag.DICT -> readDict()
                else -> throw DtxException("Unknown object archive tag: $tag")
            }

        private fun readArray(): NSObject {
            val count = readU32()
            val values = ArrayList<NSObject>(count)
            repeat(count) { values.add(readObject()) }
            return NSObject.array(values)
        }

        private fun readDict(): NSObject {
            val count = readU32()
            val values = LinkedHashMap<String, NSObject>()
            repeat(count) {
                val key = String(readBytes(), StandardCharsets.UTF_8)
                values[key] = readObject()
            }
            return NSObject.dict(values)
        }

        private fun readByte(): Byte {
            if (offset >= bytes.size)
                throw DtxException("Unexpected EOF while reading object archive.")
            return bytes[offset++]
        }

        private fun readU32(): Int {
            if (offset + 4 > bytes.size)
                throw DtxException("Unexpected EOF while reading u32.")
            val value = u32At(bytes, offset)
            offset += 4
            return value
        }

        private fun readU64(): Long {
            if (offset + 8 > bytes.size)
                throw DtxException("Unexpected EOF while reading u64.")
            val value = u64At(bytes, offset)
            offset += 8
            return value
        }

        private fun readBytes(): ByteArray {
            val length = readU32()
            if (offset + length > bytes.size)
                throw DtxException("Object archive byte payload extends past buffer.")
            val out = bytes.copyOfRange(offset, offset + length)
            offset += length
            return out
        }
    }
}

internal fun writeU32(out: ByteArrayOutputStream, value: Int) {
    out.write(value and 0xff)
    out.write((value ushr 8) and 0xff)
    out.write((value ushr 16) and 0xff)
    out.write((value ushr 24) and 0xff)
}

internal fun writeU64(out: ByteArrayOutputStream, value: Long) {
    var current = value
    repeat(8) {
        out.write((current and 0xffL).toInt())
        current = current ushr 8
    }
}

internal fun u32At(bytes: ByteArray, offset: Int): Int =
    (bytes[offset].toInt() and 0xff) or
        ((bytes[offset + 1].toInt() and 0xff) shl 8) or
        ((bytes[offset + 2].toInt() and 0xff) shl 16) or
        ((bytes[offset + 3].toInt() and 0xff) shl 24)

internal fun u64At(bytes: ByteArray, offset: Int): Long {
    var value = 0L
    for (i in 7 downTo 0)
        value = (value shl 8) or (bytes[offset + i].toLong() and 0xffL)
    return value
}

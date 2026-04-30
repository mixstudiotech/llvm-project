package org.llvm.dtx

sealed class NSObject {
    data object Null : NSObject()
    data class BoolValue(val value: Boolean) : NSObject()
    data class IntValue(val value: Long) : NSObject()
    data class UIntValue(val value: Long) : NSObject()
    data class DoubleValue(val value: Double) : NSObject()
    data class StringValue(val value: String) : NSObject()
    class DataValue(value: ByteArray) : NSObject() {
        val value: ByteArray = value.copyOf()

        override fun equals(other: Any?): Boolean =
            other is DataValue && value.contentEquals(other.value)

        override fun hashCode(): Int = value.contentHashCode()
        override fun toString(): String = "<data:${value.size}>"
    }
    data class ArrayValue(val value: List<NSObject>) : NSObject()
    data class DictValue(val value: Map<String, NSObject>) : NSObject()

    fun asString(): String =
        (this as? StringValue)?.value ?: throw DtxException("NSObject is not a string.")

    companion object {
        fun bool(value: Boolean): NSObject = BoolValue(value)
        fun int(value: Long): NSObject = IntValue(value)
        fun uint(value: Long): NSObject = UIntValue(value)
        fun double(value: Double): NSObject = DoubleValue(value)
        fun string(value: String): NSObject = StringValue(value)
        fun data(value: ByteArray): NSObject = DataValue(value)
        fun array(value: Iterable<NSObject>): NSObject = ArrayValue(value.toList())
        fun dict(value: Map<String, NSObject>): NSObject = DictValue(value.toSortedMap())
        fun from(value: Boolean): NSObject = bool(value)
        fun from(value: Int): NSObject = int(value.toLong())
        fun from(value: Long): NSObject = int(value)
        fun from(value: String): NSObject = string(value)
        fun from(value: Double): NSObject = double(value)
        fun from(value: ByteArray): NSObject = data(value)
    }
}

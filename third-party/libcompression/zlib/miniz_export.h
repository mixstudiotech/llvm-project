#ifndef MINIZ_EXPORT_H
#define MINIZ_EXPORT_H

/* Statically linked into libcompression: no symbol export decoration. */
#ifndef MINIZ_EXPORT
#define MINIZ_EXPORT
#endif

#ifndef MINIZ_NO_EXPORT
#define MINIZ_NO_EXPORT
#endif

#ifndef MINIZ_DEPRECATED
#define MINIZ_DEPRECATED
#endif

#ifndef MINIZ_DEPRECATED_EXPORT
#define MINIZ_DEPRECATED_EXPORT MINIZ_EXPORT MINIZ_DEPRECATED
#endif

#ifndef MINIZ_DEPRECATED_NO_EXPORT
#define MINIZ_DEPRECATED_NO_EXPORT MINIZ_NO_EXPORT MINIZ_DEPRECATED
#endif

#endif /* MINIZ_EXPORT_H */

/* Hand-written replacement for the header miniz's CMake normally generates.
   ps5-unarchiver links miniz statically, so every symbol has default linkage. */
#ifndef MINIZ_EXPORT_H
#define MINIZ_EXPORT_H

#define MINIZ_EXPORT
#define MINIZ_NO_EXPORT

#endif /* MINIZ_EXPORT_H */

# v5.15.40 changes

- Moved all compile-time feature switches into `src/config/AppConfig.h`.
- Removed the reverse include from `AppConfig.h` into the Arduino `.ino`.
- Removed the profile-only preprocessor guard around the sketch.
- The sketch and normal C++ modules now include configuration in one direction.
- Updated firmware messages, documentation, and project validation for the simpler layout.

# Android library format test

From the repository root, with a JDK and `kotlinc` on the path:

```sh
model=src/android/app/src/main/java/io/github/zortos293/lichii/model
tmp=$(mktemp -d)
kotlinc "$model/GameFileType.kt" src/android/tests/GameFileTypeTest.kt -include-runtime -d "$tmp/test.jar" &&
  java -jar "$tmp/test.jar"
```

The test uses synthetic document display names. It checks which names the Android frontend passes
to native game validation and external-content registration; it does not open or boot a game.

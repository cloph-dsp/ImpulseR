#!/bin/sh

# Gradle wrapper script
# Licensed under the Apache License, Version 2.0

# Resolve symlinks
SCRIPT="$0"
while [ -h "$SCRIPT" ]; do
  DIR="$( cd "$( dirname "$SCRIPT" )" && pwd )"
  SCRIPT="$( readlink "$SCRIPT" )"
  [ "${SCRIPT:0:1}" = "/" ] || SCRIPT="$DIR/$SCRIPT"
done
DIR="$( cd "$( dirname "$SCRIPT" )" && pwd )"

APP_HOME="$DIR"
CLASSPATH="$APP_HOME/gradle/wrapper/gradle-wrapper.jar"

# Find java
if [ -n "$JAVA_HOME" ]; then
  JAVACMD="$JAVA_HOME/bin/java"
else
  JAVACMD="java"
fi

exec "$JAVACMD" $DEFAULT_JVM_OPTS $JAVA_OPTS $GRADLE_OPTS -classpath "$CLASSPATH" org.gradle.wrapper.GradleWrapperMain "$@"

#!/bin/sh

# Gradle wrapper script - POSIX compatible

# Resolve symlinks
SCRIPT=$0
while [ -h "$SCRIPT" ]; do
  DIR=$( cd "$( dirname "$SCRIPT" )" && pwd )
  SCRIPT=$( readlink "$SCRIPT" )
  case "$SCRIPT" in /*) ;; *) SCRIPT="$DIR/$SCRIPT" ;; esac
done
DIR=$( cd "$( dirname "$SCRIPT" )" && pwd )

APP_HOME="$DIR"
CLASSPATH="$APP_HOME/gradle/wrapper/gradle-wrapper.jar"

# Find java
if [ -n "$JAVA_HOME" ]; then
  JAVACMD="$JAVA_HOME/bin/java"
else
  JAVACMD="java"
fi

exec "$JAVACMD" $DEFAULT_JVM_OPTS $JAVA_OPTS $GRADLE_OPTS -classpath "$CLASSPATH" org.gradle.wrapper.GradleWrapperMain "$@"

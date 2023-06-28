#!/bin/bash

#version rule:MAJORVERSION.MINORVERSION.COMMIT_COUNT-g(COMMIT_ID)

BASE=$(pwd)
echo $BASE

#major version
MAJORVERSION=1

#current minor version,if release a new version,please update it
MINORVERSION=5

#release version commit id,please store commit id here every release version
RELEASE_COMMIT_ID=6f9fa9fa

#modue name/
MODULE_NAME=MM-module-name:gst-plugin-aml-asink

#get all commit count from last release version
COMMIT_COUNT=$(git rev-list $RELEASE_COMMIT_ID..HEAD --count)
echo commit count $COMMIT_COUNT

#get current commit id
COMMIT_ID=$(git rev-parse --short HEAD)
echo commit id $COMMIT_ID

#find the module name line
MODULE_NAME_LINE=`sed -n '/\"MM-module-name/=' src/aml_version.h`
#echo $VERSION_LINE

#version rule string
VERSION_STRING=${MAJORVERSION}.${MINORVERSION}.${COMMIT_COUNT}-g${COMMIT_ID}
echo version: $VERSION_STRING

#update the original version
if [ ${MODULE_NAME_LINE} -gt 0 ]; then
sed -i -e ${MODULE_NAME_LINE}s"/.*/\"${MODULE_NAME},version:${VERSION_STRING}\"\;/" src/aml_version.h
fi

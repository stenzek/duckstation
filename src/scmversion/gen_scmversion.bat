@echo off

SET VERSIONFILE="scmversion.cpp"
FOR /F "tokens=* USEBACKQ" %%g IN (`git rev-parse --abbrev-ref HEAD`) do (SET "BRANCH=%%g")
FOR /F "tokens=* USEBACKQ" %%g IN (`git describe --tags --dirty --exclude latest`) do (SET "TAG=%%g")

SET SIGNATURELINE=// %BRANCH% %TAG%
SET /P EXISTINGLINE=< %VERSIONFILE%

IF "%EXISTINGLINE%"=="%SIGNATURELINE%" (
  ECHO Signature matches, skipping writing %VERSIONFILE%
  EXIT
)

ECHO Updating %VERSIONFILE%...

(ECHO %SIGNATURELINE%
ECHO const char* g_scm_branch_str = "%BRANCH%";
ECHO const char* g_scm_tag_str = "%TAG%";
)>%VERSIONFILE%

EXIT


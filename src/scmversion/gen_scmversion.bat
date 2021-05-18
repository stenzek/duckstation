@echo off

SET VERSIONFILE="scmversion.cpp"
FOR /F "tokens=* USEBACKQ" %%g IN (`git rev-parse HEAD`) do (SET "HASH=%%g")
FOR /F "tokens=* USEBACKQ" %%g IN (`git rev-parse --abbrev-ref HEAD`) do (SET "BRANCH=%%g")
FOR /F "tokens=* USEBACKQ" %%g IN (`git describe --tags --dirty --exclude latest --exclude preview --exclude play-store-release`) do (SET "TAG=%%g")
FOR /F "tokens=* USEBACKQ" %%g IN (`git log -1 --date=iso8601-strict "--format=%%cd"`) do (SET "CDATE=%%g")

SET SIGNATURELINE=// %HASH% %BRANCH% %TAG% %CDATE%
SET /P EXISTINGLINE=< %VERSIONFILE%

IF "%EXISTINGLINE%"=="%SIGNATURELINE%" (
  ECHO Signature matches, skipping writing %VERSIONFILE%
  EXIT
)

ECHO Updating %VERSIONFILE%...

(ECHO %SIGNATURELINE%
ECHO const char* g_scm_hash_str = "%HASH%";
ECHO const char* g_scm_branch_str = "%BRANCH%";
ECHO const char* g_scm_tag_str = "%TAG%";
ECHO const char* g_scm_date_str = "%CDATE%";
)>%VERSIONFILE%

EXIT


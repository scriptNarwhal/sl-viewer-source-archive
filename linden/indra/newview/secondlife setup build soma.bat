@rem Invoke the script which preps then runs the installer.
@rem This batch file is customized per grid.

set LOGINPAGEURL=http://secondlife.com/app/login/beta/
set ARGS=/DSOMA

"secondlife setup build.bat" %LOGINPAGEURL% %ARGS%
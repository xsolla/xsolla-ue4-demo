import os
import os.path
import subprocess
import sys
import shutil
import git
import stat
import platform
import getopt
import winreg

# Prerequisites:
#
# - Git (https://git-scm.com/downloads)
# - Python 3.8+ (https://www.python.org/downloads/)
# - GitPython library (https://gitpython.readthedocs.io/en/stable/intro.html#requirements)
# - Unreal Engine 4.26 (https://www.epicgames.com/store/download)
# - ReSharper Command Line Tools (https://www.jetbrains.com/resharper/download/#section=commandline)
#
# Make sure that Python and Git tools are added to your PATH so you can freely invoke them on the command line.
# Unreal Engine install has to be properly configured (this means that building for Windows and Android works).

# Demo project is pre-configured to work with particular plugin.
# In order to use this script with different plugins corresponding project settings should be modified.
# List of plugin-dependent settings:
# - enabled plugins
# - default game level

# Global variables
engine_path = None                  # path to UE install directory
demo_project_path = None            # path to demo project directory
demo_project_name = None            # demo project name
build_output_path = None            # path to build artifacts direcory
plugin_repo_link = None             # UE plugin repo link
plugin_repo_branch = None           # UE plugin repo branch
inspect_tool_path = None            # path to code inspection tool
inspect_artifact_path = None        # path to code inspection artifact

# Plugin git clone progress tracking utility
class CloneProgress(git.remote.RemoteProgress):
    def update(self, op_code, cur_count, max_count=None, message=''):
        print(self._cur_line)

# Delete read-only files utility
def DeleteReadOnly(action, name, exc):
    os.chmod(name, stat.S_IWRITE)
    os.remove(name)

# File writing utility
def writeFile(filename, data):
    with open(filename, 'wb') as f:
        f.write(data.encode('utf-8'))

# Get custom batch files folder path utility
def getCustomBatchScriptDir():
    scriptDir = os.path.join(os.environ['HOMEDRIVE'] + os.environ['HOMEPATH'], '.ue4')
    try:
        os.makedirs(scriptDir)
    except:
        pass    
    return scriptDir

# Get project files generation script utility
def getGenerationScript():           
    try:
        # Query the shell integration to determine the location of the UnrealVersionSelector.exe, which generates VS project files
        key = winreg.OpenKey(winreg.HKEY_CLASSES_ROOT, 'Unreal.ProjectFile\\shell\\rungenproj\\command')
        if key:
            command = winreg.QueryValue(key, None)
            if len(command) > 0:                
                # Write the command to run UnrealVersionSelector.exe to custom batch file
                customBat = os.path.join(getCustomBatchScriptDir(), 'GenerateProjectFiles.bat')
                writeFile(customBat, command.replace('"%1"', '%1') + '\r\n')
                return customBat
    except:
        pass
    
    raise Exception('Could not detect the location of UnrealVersionSelector.exe.')

# Check script args
if len(sys.argv) < 8:
    print('Provide the following parameters:')
    print('-u, --ue_base - path to the UE installation directory')
    print('-d, --demo_base - path to the demo project directory')
    print('-n, --demo_name - demo project name')
    print('-o, --build_output - path to build artifacts direcory')
    print('-r, --repo - UE plugin repo link')
    print('-b, --branch - UE plugin repo branch')
    print('-i, --inspect_tool - path to code inspection tool')
    print('-a, --inspect_artifact - path to code inspection artifact')
    sys.exit('Invalid parameter provided')

# Parse script args
argv = sys.argv[1:]
try:
    opts, args = getopt.getopt(argv, "u:d:n:o:r:b:i:a:", ["ue_base=", "demo_base=", "demo_name=", "build_output=", "repo=", "branch=", "inspect_tool=", "inspect_artifact="])
except:
    sys.exit('Error: Failed to parse arguments')

for opt, arg in opts:
    if opt in ['-u', '--ue_base']:
        engine_path = os.path.abspath(arg)
    elif opt in ['-d', '--demo_base']:
        demo_project_path = os.path.abspath(arg)
    elif opt in ['-n', '--demo_name']:
        demo_project_name = arg
    elif opt in ['-o', '--build_output']:
        build_output_path = os.path.abspath(arg)
    elif opt in ['-r', '--repo']:
        plugin_repo_link = arg
    elif opt in ['-b', '--branch']:
        plugin_repo_branch = arg
    elif opt in ['-i', '--inspect_tool']:
        inspect_tool_path = os.path.abspath(arg)
    elif opt in ['-a', '--inspect_artifact']:
        inspect_artifact_path = os.path.abspath(arg)

# Check whether all variables are set
if engine_path is None:
    sys.exit('Error: Provide a valid path to the UE installation directory')
if demo_project_path is None:
    sys.exit('Error: Provide a valid path to the demo project directory')
if demo_project_name is None:
    sys.exit('Error: Provide a valid demo project name')
if build_output_path is None:
    sys.exit('Error: Provide a valid path to the directory where build artifacts will be stored')
if plugin_repo_link is None:
    sys.exit('Error: Provide a valid UE plugin repo link')
if plugin_repo_branch is None:
    sys.exit('Error: Provide a valid UE plugin repo branch')
if inspect_tool_path is None:
    sys.exit('Error: Provide a valid path to code inspection tool')
if inspect_artifact_path is None:
    sys.exit('Error: Provide a valid path to code inspection artifact')

# Check platform for running this script (currently only Windows supported)
if not platform.system() == 'Windows':
    sys.exit(f'Error: {platform.system()} is not supported')

# Check if demo project exists
if not os.path.exists(demo_project_path):
    sys.exit(f'Error: Failed to locate demo project at {demo_project_path}')

# Clear temporaries if any in demo project folder
temp_folders = ['Binaries', 'Build', 'Intermediate', 'DerivedDataCache', 'Saved', 'Plugins', '.vs']
for temp_folder in temp_folders:
    temp_folder_path = os.path.join(demo_project_path, temp_folder)
    if (os.path.exists(temp_folder_path)):
        print(f'Removing {temp_folder_path}')
        shutil.rmtree(temp_folder_path, ignore_errors=False, onerror=DeleteReadOnly)

# Clone UE plugin to demo project Plugins folder
from git import Repo
repo = Repo.clone_from(plugin_repo_link, os.path.join(demo_project_path, 'Plugins/Xsolla'), branch=plugin_repo_branch, progress=CloneProgress())

# Check if Unreal Automation Tool (UAT) exists
uat = os.path.join(engine_path, 'Engine/Binaries/DotNET/AutomationTool.exe')
if not os.path.exists(uat):
    sys.exit(f'Error: Failed to locate Unreal Build Tool at {uat}')

# Prepare folder for storing build artifacts
packages_path = os.path.join(build_output_path, 'Packages')
if os.path.exists(packages_path):
    shutil.rmtree(packages_path)
os.makedirs(packages_path)

# Package demo project
demo_project = os.path.join(demo_project_path, demo_project_name + '.uproject')
build_platforms = ['Win64', 'Android']
for platform in build_platforms:
    result = subprocess.run([uat,
        'BuildCookRun',
        '-utf8output',
        '-platform=' + platform,
        '-project=' + demo_project,
        '-noP4',
        '-cook',
        '-build',
        '-stage',
        '-prereqs',
        '-archivedirectory=' + packages_path,
        '-archive',
    ], stdout=sys.stdout)

    if result.returncode != 0:
        print(f'Error: AutomationTool Error: {result.stderr}')
        break

# Generate IDE (Visual Studio) project files for demo project
genScript = getGenerationScript()
generation = subprocess.run([genScript, demo_project], cwd=os.path.dirname(genScript), stdout=sys.stdout)

# Prepare folder for storing code inspection artifacts
inspect_path = os.path.join(inspect_artifact_path, 'Inspect')
if os.path.exists(inspect_path):
    shutil.rmtree(inspect_path)
os.makedirs(inspect_path)

# Run code inspection
demo_project_sln = os.path.join(demo_project_path, demo_project_name + '.sln')
inspection = subprocess.run([inspect_tool_path, demo_project_sln, '-o=' + os.path.join(inspect_path, 'InspectResult.xml'), '--project=' + demo_project_name], stdout=sys.stdout)
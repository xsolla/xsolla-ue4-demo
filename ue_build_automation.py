import os
import os.path
import subprocess
import sys
import shutil
import git
import stat
import platform

# Prerequisites:
#
# - Git (https://git-scm.com/downloads)
# - Python 3.8+ (https://www.python.org/downloads/)
# - GitPython library (https://gitpython.readthedocs.io/en/stable/intro.html#requirements)
# - Unreal Engine 4.26 (https://www.epicgames.com/store/download)
#
# Make sure that Python and Git tools are added to your PATH so you can freely invoke them on the command line.
# Unreal Engine install has to be properly configured (this means that building for Windows and Android works).

# Demo project is pre-configured to work with particular plugin.
# In order to use this script with different plugins corresponding project settings should be modified.
# List of plugin-dependent settings:
# - enabled plugins
# - default game level

# Global variables

engine_path = 'D:/UnrealEditors/UE_4.26'                            # Unreal Engine install root folder
demo_project_path = 'D:/Test'                                       # path to demo project root folder
demo_project_name = 'MyXsolla'                                      # demo project name
build_output_path = 'C:/Users/tusta/Desktop'                        # folder where build artifacts will be stored
plugin_repo_link = 'https://github.com/xsolla/store-ue4-sdk.git'    # plugin repo for which demo builds should be produced
plugin_repo_branch = 'develop'                                      # plugin repo branch that should be checked out

# Plugin git clone progress tracking utility
class CloneProgress(git.remote.RemoteProgress):
    def update(self, op_code, cur_count, max_count=None, message=''):
        print(self._cur_line)

# Delete read-only files utility
def DeleteReadOnly(action, name, exc):
    os.chmod(name, stat.S_IWRITE)
    os.remove(name)

# Check platform for running this script (currently only Windows supported)
if not platform.system() == 'Windows':
    sys.exit(f'Error: {platform.system()} is not supported')

# Check if demo project exists
if not os.path.exists(demo_project_path):
    sys.exit(f'Error: Failed to locate demo project at {demo_project_path}')

# Clear temporaries if any in demo project folder
temp_folders = ['Binaries', 'Build', 'Intermediate', 'DerivedDataCache', 'Saved', 'Plugins']
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
build_platforms = ['Win64', 'Android']
demo_project = os.path.join(demo_project_path, demo_project_name + '.uproject')
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
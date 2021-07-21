import os
import os.path
import subprocess
import sys
import shutil
import git
import stat
import platform
import getopt

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
engine_path = None                  # path to UE install directory
demo_project_path = None            # path to demo project directory
demo_project_name = None            # demo project name
build_output_path = None            # path to the directory where build artifacts will be stored
plugin_repo_link = None             # UE plugin repo link
plugin_repo_branch = None           # UE plugin repo branch

# Plugin git clone progress tracking utility
class CloneProgress(git.remote.RemoteProgress):
    def update(self, op_code, cur_count, max_count=None, message=''):
        print(self._cur_line)

# Delete read-only files utility
def DeleteReadOnly(action, name, exc):
    os.chmod(name, stat.S_IWRITE)
    os.remove(name)

# Check script args
if len(sys.argv) < 6:
    print('Provide the following parameters:')
    print('-u, --ue_base - path to the UE installation directory')
    print('-d, --demo_base - path to the demo project directory')
    print('-n, --demo_name - demo project name')
    print('-o, --build_output - path to the directory where build artifacts will be stored')
    print('-r, --repo - UE plugin repo link')
    print('-b, --branch - UE plugin repo branch')
    sys.exit('Invalid parameter provided')

# Parse script args
argv = sys.argv[1:]
try:
    opts, args = getopt.getopt(argv, "u:d:n:o:r:b:", ["ue_base=", "demo_base=", "demo_name=", "build_output=", "repo=", "branch="])
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
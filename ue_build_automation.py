import platform
import os
import os.path
import subprocess
import sys

engine_path = 'D:/UnrealEditors/UE_4.26'
uat = os.path.join(engine_path, 'Engine/Binaries/DotNET/AutomationTool.exe')

if not os.path.exists(uat):
    print(f'Error: Failed to locate Unreal Build Tool at {uat}')

demo_project = 'D:/UnrealProjects/XsollaStoreDemo/MyXsolla.uproject'

platform_name = 'Win64'
client_config = 'Shipping'
dest_dir = 'C:/Users/tusta/Desktop'

result = subprocess.run([uat, 'BuildCookRun',
 '-utf8output',
 '-platform=' + platform_name,
 '-clientconfig=' + client_config,
 '-project=' + demo_project,
 '-noP4',
 '-cook',
 '-build',
 '-stage',
 '-prereqs',
 '-archivedirectory=' + dest_dir,
 '-archive',],
 stdout=sys.stdout)


if result.returncode != 0:
    print('Error: AutomationTool Error: ' + result.stderr)
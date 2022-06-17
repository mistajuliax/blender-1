# ##### BEGIN GPL LICENSE BLOCK #####
#
#  This program is free software; you can redistribute it and/or
#  modify it under the terms of the GNU General Public License
#  as published by the Free Software Foundation; either version 2
#  of the License, or (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program; if not, write to the Free Software Foundation,
#  Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
#
# ##### END GPL LICENSE BLOCK #####

# <pep8 compliant>

# Runs on buildbot slave, creating a release package using the build
# system and zipping it into buildbot_upload.zip. This is then uploaded
# to the master in the next buildbot step.

import os
import subprocess
import sys
import zipfile

# get builder name
if len(sys.argv) < 2:
    sys.stderr.write("Not enough arguments, expecting builder name\n")
    sys.exit(1)

builder = sys.argv[1]
# Never write branch if it is master.
branch = sys.argv[2] if (len(sys.argv) >= 3 and sys.argv[2] != 'master') else ''

# scons does own packaging
if builder.find('scons') != -1:
    python_bin = 'python'
    if builder.find('linux') != -1:
        python_bin = '/opt/lib/python-2.7/bin/python2.7'

    os.chdir('../blender.git')
    scons_options = [
        'BF_QUICK=slnt',
        f'BUILDBOT_BRANCH={branch}',
        'buildslave',
        'BF_FANCY=False',
    ]


    buildbot_dir = os.path.dirname(os.path.realpath(__file__))
    config_dir = os.path.join(buildbot_dir, 'config')
    build_dir = os.path.join('..', 'build', builder)
    install_dir = os.path.join('..', 'install', builder)

    if builder.find('linux') == -1:
        if builder.find('win') != -1:
            bitness = '64' if builder.find('win64') != -1 else '32'
            scons_options.append(f'BF_INSTALLDIR={install_dir}')
            scons_options.extend(
                (
                    f'BF_BUILDDIR={build_dir}',
                    f'BF_BITNESS={bitness}',
                    'WITH_BF_CYCLES_CUDA_BINARIES=True',
                    'BF_CYCLES_CUDA_NVCC=nvcc.exe',
                )
            )

            if builder.find('mingw') != -1:
                scons_options.append('BF_TOOLSET=mingw')
            if builder.endswith('vc2013'):
                scons_options.extend(('MSVS_VERSION=12.0', 'MSVC_VERSION=12.0'))
        elif builder.find('mac') != -1:
            config = (
                'user-config-mac-x86_64.py'
                if builder.find('x86_64') != -1
                else 'user-config-mac-i386.py'
            )

            scons_options.append(f'BF_CONFIG={os.path.join(config_dir, config)}')

        retcode = subprocess.call([python_bin, 'scons/scons.py'] + scons_options)
    else:
        scons_options += [
            'WITH_BF_NOBLENDER=True',
            'WITH_BF_PLAYER=False',
            f'BF_BUILDDIR={build_dir}',
            f'BF_INSTALLDIR={install_dir}',
            'WITHOUT_BF_INSTALL=True',
        ]


        config = None
        bits = None

        if builder.endswith('linux_glibc211_x86_64_scons'):
            config = 'user-config-glibc211-x86_64.py'
            chroot_name = 'buildbot_squeeze_x86_64'
            bits = 64
        elif builder.endswith('linux_glibc211_i386_scons'):
            config = 'user-config-glibc211-i686.py'
            chroot_name = 'buildbot_squeeze_i686'
            bits = 32

        if config is not None:
            config_fpath = os.path.join(config_dir, config)
            scons_options.append(f'BF_CONFIG={config_fpath}')

        blender = os.path.join(install_dir, 'blender')
        blenderplayer = os.path.join(install_dir, 'blenderplayer')
        subprocess.call(['schroot', '-c', chroot_name, '--', 'strip', '--strip-all', blender, blenderplayer])

        extra = '/' + os.path.join('home', 'sources', 'release-builder', 'extra')
        mesalibs = os.path.join(extra, f'mesalibs{str(bits)}.tar.bz2')
        software_gl = os.path.join(extra, 'blender-softwaregl')

        os.system(f'tar -xpf {mesalibs} -C {install_dir}')
        os.system(f'cp {software_gl} {install_dir}')
        os.system(f"chmod 755 {os.path.join(install_dir, 'blender-softwaregl')}")

        retcode = subprocess.call(['schroot', '-c', chroot_name, '--', python_bin, 'scons/scons.py'] + scons_options)

    sys.exit(retcode)
elif 'win' in builder:
    files = [f for f in os.listdir('.') if os.path.isfile(f) and f.endswith('.zip')]
    for f in files:
        os.remove(f)
    retcode = subprocess.call(['cpack', '-G', 'ZIP'])
    result_file = [f for f in os.listdir('.') if os.path.isfile(f) and f.endswith('.zip')][0]
    os.rename(result_file, f"{builder}.zip")
        # create zip file
    try:
        upload_zip = "buildbot_upload.zip"
        if os.path.exists(upload_zip):
            os.remove(upload_zip)
        z = zipfile.ZipFile(upload_zip, "w", compression=zipfile.ZIP_STORED)
        z.write(f"{builder}.zip")
        z.close()
        sys.exit(retcode)
    except Exception as ex:
        sys.stderr.write(f'Create buildbot_upload.zip failed{str(ex)}' + '\n')
        sys.exit(1)


# clean release directory if it already exists
release_dir = 'release'

if os.path.exists(release_dir):
    for f in os.listdir(release_dir):
        if os.path.isfile(os.path.join(release_dir, f)):
            os.remove(os.path.join(release_dir, f))

# create release package
try:
    subprocess.call(['make', 'package_archive'])
except Exception as ex:
    sys.stderr.write(f'Make package release failed{str(ex)}' + '\n')
    sys.exit(1)

# find release directory, must exist this time
if not os.path.exists(release_dir):
    sys.stderr.write("Failed to find release directory %r.\n" % release_dir)
    sys.exit(1)

# find release package
file = None
filepath = None

for f in os.listdir(release_dir):
    rf = os.path.join(release_dir, f)
    if os.path.isfile(rf) and f.startswith('blender'):
        file = f
        filepath = rf

if not file:
    sys.stderr.write("Failed to find release package.\n")
    sys.exit(1)

# create zip file
try:
    upload_zip = "buildbot_upload.zip"
    if os.path.exists(upload_zip):
        os.remove(upload_zip)
    z = zipfile.ZipFile(upload_zip, "w", compression=zipfile.ZIP_STORED)
    z.write(filepath, arcname=file)
    z.close()
except Exception as ex:
    sys.stderr.write(f'Create buildbot_upload.zip failed{str(ex)}' + '\n')
    sys.exit(1)

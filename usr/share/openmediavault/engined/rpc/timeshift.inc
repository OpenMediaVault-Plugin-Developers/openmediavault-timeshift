<?php
/**
 * Copyright (C) 2024-2026 openmediavault plugin developers
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

class OMVRpcServiceTimeshift extends \OMV\Rpc\ServiceAbstract
{
    public function getName()
    {
        return "Timeshift";
    }

    public function initialize()
    {
        $this->registerMethod("getSettings");
        $this->registerMethod("setSettings");
        $this->registerMethod("getBackupDeviceList");

        $this->registerMethod("getSnapshotList");
        $this->registerMethod("getAltSnapshotList");
        $this->registerMethod("createSnapshot");
        $this->registerMethod("deleteSnapshot");
        $this->registerMethod("revertSnapshot");
    }

    public function getSettings($params, $context)
    {
        // Validate the RPC caller context.
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        // Get the configuration object.
        $db = \OMV\Config\Database::getInstance();
        $object = $db->get("conf.service.timeshift");
        return $object->getAssoc();
    }

    public function setSettings($params, $context)
    {
        // Validate the RPC caller context.
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        // Validate the parameters of the RPC service method.
        $this->validateMethodParams($params, "rpc.timeshift.setsettings");
        // validate btrfs prerequisites when btrfs mode is requested
        if ($params['btrfsmode']) {
            $output = [];
            $cmd = new \OMV\System\Process(['findmnt', '-n', '-o', 'FSTYPE', '/']);
            $cmd->setQuiet(true);
            $cmd->execute($output, $exitStatus);
            if (trim(implode('', $output)) !== 'btrfs') {
                throw new \OMV\Exception(gettext("BTRFS mode requires the root filesystem (/) to be formatted as BTRFS."));
            }
            $output = [];
            $cmd = new \OMV\System\Process(['findmnt', '-n', '-o', 'OPTIONS', '/']);
            $cmd->setQuiet(true);
            $cmd->execute($output, $exitStatus);
            if (!preg_match('/(^|,)subvol=\/@(,|$)/', implode('', $output))) {
                throw new \OMV\Exception(gettext("BTRFS mode requires the root filesystem to be mounted as the '@' subvolume (subvol=@ or subvol=/@ in /etc/fstab)."));
            }
            $output = [];
            $cmd = new \OMV\System\Process(['btrfs', 'subvolume', 'get-default', '/']);
            $cmd->setQuiet(true);
            $cmd->execute($output, $exitStatus);
            if ($exitStatus !== 0 || !preg_match('/^ID 5\b/', trim(implode('', $output)))) {
                throw new \OMV\Exception(gettext("BTRFS mode requires the default subvolume to be the top level (ID 5). Run 'btrfs subvolume set-default 5 /' and try again."));
            }
            // in BTRFS mode timeshift stores snapshots on the root filesystem's
            // own device, so always store the root device's UUID as the backup
            // device regardless of what (if anything) is selected in the form;
            // otherwise 'backup_device_uuid' can end up empty and timeshift
            // reports "Device: Not Selected" and lists no snapshots at all.
            $output = [];
            $cmd =new \OMV\System\Process(['findmnt', '-n', '-o', 'UUID', '/']);
            $cmd->setQuiet(true);
            $cmd->execute($output, $exitStatus);
            $rootUuid = trim(implode('', $output));
            if ($rootUuid === '') {
                throw new \OMV\Exception(gettext("Failed to determine the UUID of the root filesystem."));
            }
            $params['backupdev'] = $rootUuid;
        }
        // cleanup exclude list
        $exclude = explode(",", str_replace([";", PHP_EOL], ",", $params["exclude"]));
        $excludeT = array_map("trim", $exclude);
        $excludeF = array_filter($excludeT);
        $excludeU = array_unique($excludeF);
        $params["exclude"] = implode(",", $excludeU);
        // Get the existing configuration object.
        $db = \OMV\Config\Database::getInstance();
        $object = $db->get("conf.service.timeshift");
        $object->setAssoc($params);
        $db->set($object);
        // Return the configuration object.
        return $object->getAssoc();
    }

    public function getBackupDeviceList($params, $context)
    {
        // Validate the RPC caller context.
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        // list backup devices
        $cmdArgs = [];
        $cmdArgs[] = 'blkid';
        $cmdArgs[] = '-o export';
        $cmd = new \OMV\System\Process($cmdArgs);
        $cmd->setQuiet(true);
        $cmd->execute($output, $exitStatus);
        $output[] = '';
        // build return list filtering out bad filesystems
        $badfs = ['zfs_member', 'swap', 'LVM2_member', 'vfat'];
        $filesystems = [];
        $devname = $uuid = $fstype = null;
        foreach ($output as $line) {
            if (trim($line) === '') {
                if (in_array($fstype, $badfs, true) || strpos($devname, '/dev/zram') === 0)
                    continue;
                if ($devname !== null && $uuid !== null) {
                    $filesystems[] = [
                        'uuid' => $uuid,
                        'devname' => $devname,
                        'description' => sprintf('%s [%s] (%s)', $devname, $uuid, $fstype ?? 'unknown'),
                    ];
                }
                $devname = $uuid = $fstype = null;
                continue;
            }
            list($key, $value) = explode('=', $line, 2);
            switch ($key) {
                case 'DEVNAME':
                    $devname = $value;
                    break;
                case 'UUID':
                    $uuid = $value;
                    break;
                case 'TYPE':
                    $fstype = $value;
                    break;
            }
        }
        // sort and return the result
        usort($filesystems, static function(array $a, array $b): int {
            return strcmp($a['devname'], $b['devname']);
        });
        return ($filesystems);
    }

    public function getSnapshotList($params, $context)
    {
        // Validate the RPC caller context.
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        // Validate the parameters of the RPC service method.
        $this->validateMethodParams($params, "rpc.common.getlist");
        // check to see if backup device is set (not required in BTRFS mode,
        // which uses the root filesystem's own BTRFS device)
        $cfg = '/etc/timeshift/timeshift.json';
        if (!file_exists($cfg)) {
            throw new \OMV\ExecException(gettext("Backup device needs to be set and applied!"));
        }
        $data = json_decode(file_get_contents($cfg), true);
        $btrfsMode = isset($data['btrfs_mode']) && $data['btrfs_mode'] === 'true';
        if (!$btrfsMode && (!isset($data['backup_device_uuid']) || empty($data['backup_device_uuid']))) {
            throw new \OMV\ExecException(gettext("Backup device needs to be set and applied!"));
        }
        // list snapshots
        $output = [];
        $cmdArgs = [];
        $cmdArgs[] = 'timeshift';
        $cmdArgs[] = '--list';
        $cmdArgs[] = '| awk NF';
        $cmd = new \OMV\System\Process($cmdArgs);
        $cmd->setQuiet(true);
        $cmd->execute($output, $exitStatus);
        $output = array_filter($output);
        $snapshots = [];
        if (strpos(implode('', $output), 'No snapshots found') === false) {
            $pattern = '/^(\d+)\s+>\s+(\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2})\s+([OBHDWM](?:\s*[OBHDWM])*)(?:\s{2,}(.*))?$/';
            foreach ($output as $line) {
                $match = [];
                if (preg_match($pattern, $line, $match) !== 1) continue;
                if (strlen($match[2]) == 0) continue;
                $snapshots[] = [
                    'num' => $match[1],
                    'snapshot' => $match[2],
                    'tags' => $match[3],
                    'description' => trim($match[4])
                ];
            }
        }
        if ($btrfsMode) {
            $this->markBtrfsSubvolumes($snapshots, '/');
        }
        // Filter the result.
        return $this->applyFilter($snapshots, $params['start'], $params['limit'],
            $params['sortfield'], $params['sortdir']);
    }

    // Snapshot names (as array keys) that have an '@home' subvolume.
    private function listBtrfsHomeSnapshots(string $path): ?array
    {
        $cmdArgs = ['btrfs', 'subvolume', 'list', $path];
        $cmd = new \OMV\System\Process($cmdArgs);
        $cmd->setQuiet(true);
        $cmd->execute($output, $exitStatus);
        if ($exitStatus !== 0) {
            return null;
        }
        $homeSnapshots = [];
        foreach ($output as $line) {
            if (preg_match('#\bpath\s+timeshift-btrfs/snapshots/([^/]+)/@home\s*$#', $line, $m) === 1) {
                $homeSnapshots[$m[1]] = true;
            }
        }
        return $homeSnapshots;
    }

    // Prefix each snapshot's description with '@' or '@@h' to show which
    // subvolumes it contains.
    private function markBtrfsSubvolumes(array &$snapshots, string $path)
    {
        $homeSnapshots = $this->listBtrfsHomeSnapshots($path);
        if ($homeSnapshots === null) {
            return;
        }
        foreach ($snapshots as &$snapshot) {
            if ($snapshot['snapshot'] === 'create') continue;
            $marker = isset($homeSnapshots[$snapshot['snapshot']]) ? '@@h' : '@';
            $snapshot['description'] = ($snapshot['description'] !== '')
                ? sprintf('%s - %s', $marker, $snapshot['description'])
                : $marker;
        }
        unset($snapshot);
    }

    public function createSnapshot($params, $context)
    {
        // Validate the RPC caller context.
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        // Validate the parameters of the RPC service method.
        $this->validateMethodParams($params, "rpc.timeshift.createsnapshot");
        return $this->execBgProc(function($bgStatusFilename, $bgOutputFilename)
            use ($params) {
                // create snapshot
                $cmdArgs = [];
                $cmdArgs[] = 'timeshift';
                $cmdArgs[] = '--create';
                $cmdArgs[] = '--comment "manually created from plugin"';
                $cmdArgs[] = '--scripted';
                if ($params['snapshot'] !== '') {
                    $cmdArgs[] = sprintf("--snapshot-device '%s'", $params['device']);
                }
                $cmd = new \OMV\System\Process($cmdArgs);
                $cmd->setRedirect2to1();
                $cmdLine = $cmd->getCommandLine();
                if (0 !== $this->exec($cmdLine, $output, $bgOutputFilename)) {
                    throw new \OMV\ExecException($cmdLine, $output);
                }
                return $output;
            }
        );
    }

    public function deleteSnapshot($params, $context)
    {
        // Validate the RPC caller context.
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        // Validate the parameters of the RPC service method.
        $this->validateMethodParams($params, "rpc.timeshift.snapshot");
        if ($params['snapshot'] == 'create') {
            throw new \OMV\ExecException(gettext("This entry is used to create a snapshot only!"));
        }
        return $this->execBgProc(function($bgStatusFilename, $bgOutputFilename)
            use ($params) {
                // delete snapshot
                $cmdArgs = [];
                $cmdArgs[] = 'timeshift';
                $cmdArgs[] = '--delete';
                $cmdArgs[] = '--scripted';
                $cmdArgs[] = sprintf("--snapshot '%s'", $params['snapshot']);
                if ($params['snapshot'] !== '') {
                    $cmdArgs[] = sprintf("--snapshot-device '%s'", $params['device']);
                }
                $cmd = new \OMV\System\Process($cmdArgs);
                $cmd->setRedirect2to1();
                $cmdLine = $cmd->getCommandLine();
                if (0 !== $this->exec($cmdLine, $output, $bgOutputFilename)) {
                    throw new \OMV\ExecException($cmdLine, $output);
                }
                return $output;
            }
        );
    }

    public function revertSnapshot($params, $context)
    {
        // Validate the RPC caller context.
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        // Validate the parameters of the RPC service method.
        $this->validateMethodParams($params, "rpc.timeshift.snapshot");
        if ($params['snapshot'] == 'create') {
            throw new \OMV\ExecException(gettext("This entry is used to create a snapshot only!"));
        }
        // refuse to restore a snapshot missing '@home' when Settings expects
        // it, otherwise /home ends up unmountable after reboot
        $cfg = '/etc/timeshift/timeshift.json';
        if (file_exists($cfg)) {
            $data = json_decode(file_get_contents($cfg), true);
            $btrfsMode = isset($data['btrfs_mode']) && $data['btrfs_mode'] === 'true';
            $includeHome = isset($data['include_btrfs_home_for_restore'])
                && $data['include_btrfs_home_for_restore'] === 'true';
            if ($btrfsMode && $includeHome) {
                $homeSnapshots = $this->listBtrfsHomeSnapshots('/');
                if ($homeSnapshots !== null && !isset($homeSnapshots[$params['snapshot']])) {
                    throw new \OMV\Exception(gettext("Settings require the '@home' subvolume, but this snapshot doesn't contain it. Disable 'Include @home' in Settings or choose a different snapshot."));
                }
            }
        }
        return $this->execBgProc(function($bgStatusFilename, $bgOutputFilename)
            use ($params) {
                // revert to snapshot
                $cmdArgs = [];
                $cmdArgs[] = 'timeshift';
                $cmdArgs[] = '--restore';
                $cmdArgs[] = sprintf("--snapshot '%s'", $params['snapshot']);
                if ($params['snapshot'] !== '') {
                    $cmdArgs[] = sprintf("--snapshot-device '%s'", $params['device']);
                }
                $cmdArgs[] = '--yes';
                $cmdArgs[] = '--scripted';
                $cmd = new \OMV\System\Process($cmdArgs);
                $cmd->setRedirect2to1();
                $cmdLine = $cmd->getCommandLine();
                if (0 !== $this->exec($cmdLine, $output, $bgOutputFilename)) {
                    throw new \OMV\ExecException($cmdLine, $output);
                }
                return $output;
            }
        );
    }

    public function getAltSnapshotList($params, $context)
    {
        // Validate the RPC caller context.
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        // Validate the parameters of the RPC service method.
        $this->validateMethodParams($params, "rpc.common.getlist");
        $output = [];
        $cmdArgs = [];
        $cmdArgs[] = 'timeshift';
        $cmdArgs[] = '--list-devices';
        $cmdArgs[] = '--scripted';
        $cmdArgs[] = "| grep '>'";
        $cmdArgs[] = '| awk \'{ print $3 }\'';
        $cmd = new \OMV\System\Process($cmdArgs);
        $cmd->setQuiet(true);
        $cmd->execute($output, $exitStatus);
        $output = array_filter($output);
        // in BTRFS mode only the device holding the root filesystem can be
        // used for snapshots, so restrict the device list to that device
        $btrfsMode = false;
        $cfg = '/etc/timeshift/timeshift.json';
        if (file_exists($cfg)) {
            $data = json_decode(file_get_contents($cfg), true);
            $btrfsMode = isset($data['btrfs_mode']) && $data['btrfs_mode'] === 'true';
            if ($btrfsMode) {
                $cmd = new \OMV\System\Process(['findmnt', '-n', '-o', 'SOURCE', '/']);
                $cmd->setQuiet(true);
                $cmd->execute($rootOutput, $rootExitStatus);
                $rootDevice = trim(implode('', $rootOutput));
                // findmnt reports a subvolume mount as e.g. '/dev/sdc1[/@]';
                // strip the bracketed subvolume suffix so it compares equal
                // to the plain device path returned by --list-devices.
                $rootDevice = preg_replace('/\[.*$/', '', $rootDevice);
                $rootDeviceName = basename($rootDevice);
                $output = array_filter($output, function($dev) use ($rootDevice, $rootDeviceName) {
                    $dev = trim($dev);
                    return $dev === $rootDevice || $dev === $rootDeviceName;
                });
            }
        }
        // loop through all devices and get snapshots from each
        $snapshots = [];
        $pattern = '/^(\d+)\s+>\s+(\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2})\s+([OBHDWM](?:\s*[OBHDWM])*)(?:\s{2,}(.*))?$/';
        foreach ($output as $dev) {
            $snapshots[] = [
                'num' => '-',
                'snapshot' => 'create',
                'device' => $dev,
                'tags' => '-',
                'description' => 'use to create a snapshot on ' . $dev
            ];
            // list snapshots
            $snapoutput = [];
            $cmdArgs = [];
            $cmdArgs[] = 'timeshift';
            $cmdArgs[] = '--list';
            $cmdArgs[] = sprintf("--snapshot-device '%s'", $dev);
            $cmdArgs[] = '--scripted';
            $cmdArgs[] = '| awk NF';
            $cmd = new \OMV\System\Process($cmdArgs);
            $cmd->setQuiet(true);
            $cmd->execute($snapoutput, $exitStatus);
            $snapoutput = array_filter($snapoutput);
            if (strpos(implode('', $snapoutput), 'No snapshots found') === false) {
                foreach ($snapoutput as $line) {
                    $match = [];
                    if (preg_match($pattern, $line, $match) !== 1) continue;
                    if (strlen($match[2]) == 0) continue;
                    $snapshots[] = [
                        'num' => $match[1],
                        'snapshot' => $match[2],
                        'device' => $dev,
                        'tags' => $match[3],
                        'description' => trim($match[4])
                    ];
                }
            }
        }
        if ($btrfsMode) {
            $this->markBtrfsSubvolumes($snapshots, '/');
        }
        // Filter the result.
        return $this->applyFilter($snapshots, $params['start'], $params['limit'],
            $params['sortfield'], $params['sortdir']);
    }
}

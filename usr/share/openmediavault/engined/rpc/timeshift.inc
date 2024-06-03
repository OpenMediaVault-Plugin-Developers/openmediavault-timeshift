<?php
/**
 * Copyright (C) 2024 OpenMediaVault Plugin Developers
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
        // cleanup exclude list
        $exclude = explode(",", str_replace([";", PHP_EOL], ",", $params["exclude"]));
        $excludeT = array_map("trim", $exclude);
        $excludeF = array_filter($excludeT);
        $excludeU = array_unique($excludeF);
        sort($excludeU);
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
        $cmdArgs[] = '| grep -vE "zfs|swap"';
        $cmdArgs[] = '| awk -F \': |="|"\' \'{print $1","$3}\'';
        $cmdArgs[] = '| sort';
        $cmd = new \OMV\System\Process($cmdArgs);
        $cmd->setQuiet(true);
        $cmd->execute($output, $exitStatus);
        array_filter($output);
        $filesystems = [];
        foreach ($output as $fs) {
            $details = explode(',', $fs);
            $filesystems[] = [
                'uuid' => $details[1],
                'description' => sprintf('%s [%s]', $details[0], $details[1])
            ];
        }
        // Filter the result.
        return ($filesystems);
    }

    public function getSnapshotList($params, $context)
    {
        // Validate the RPC caller context.
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        // Validate the parameters of the RPC service method.
        $this->validateMethodParams($params, "rpc.common.getlist");
        // check to see if backup device is set
        $cfg = '/etc/timeshift/timeshift.json';
        if (!file_exists($cfg)) {
            throw new \OMV\ExecException(gettext("Backup device needs to be set and applied!"));
        }
        $data = json_decode(file_get_contents($cfg), true);
        if (!isset($data['backup_device_uuid']) || empty($data['backup_device_uuid'])) {
            throw new \OMV\ExecException(gettext("Backup device needs to be set and applied!"));
        }
        // list snapshots
        $output = [];
        $cmdArgs = [];
        $cmdArgs[] = 'timeshift';
        $cmdArgs[] = '--list';
        $cmdArgs[] = "| sed '1,10d'";
        $cmdArgs[] = '| awk NF';
        $cmd = new \OMV\System\Process($cmdArgs);
        $cmd->setQuiet(true);
        $cmd->execute($output, $exitStatus);
        array_filter($output);
        $snapshots = [];
        if (strpos(implode('', $output), 'No snapshots found') === false) {
            $pattern = '/^(\d+)\s+>\s+(\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2})\s+([OBHDWM](?:\s*[OBHDWM])*)\s*(.*)$/';
            foreach ($output as $line) {
                $match = [];
                preg_match($pattern, $line, $match);
                $snapshots[] = [
                    'num' => $match[1],
                    'snapshot' => $match[2],
                    'tags' => $match[3],
                    'description' => trim($match[4])
                ];
            }
        }
        // Filter the result.
        return $this->applyFilter($snapshots, $params['start'], $params['limit'],
            $params['sortfield'], $params['sortdir']);
    }

    public function createSnapshot($params, $context)
    {
        // Validate the RPC caller context.
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        return $this->execBgProc(function($bgStatusFilename, $bgOutputFilename)
            use ($params) {
                // delete snapshot
                $cmdArgs = [];
                $cmdArgs[] = 'timeshift';
                $cmdArgs[] = '--create';
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

    public function deleteSnapshot($params, $context)
    {
        // Validate the RPC caller context.
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        // Validate the parameters of the RPC service method.
        $this->validateMethodParams($params, "rpc.timeshift.deletesnapshot");
        // delete snapshot
        $cmdArgs = [];
        $cmdArgs[] = 'timeshift';
        $cmdArgs[] = '--delete';
        $cmdArgs[] = sprintf("--snapshot '%s'", $params['snapshot']);
        $cmd = new \OMV\System\Process($cmdArgs);
        $cmd->execute($output, $exitStatus);
        return($output);
    }

    public function revertSnapshot($params, $context)
    {
        // Validate the RPC caller context.
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        return $this->execBgProc(function($bgStatusFilename, $bgOutputFilename)
            use ($params) {
                // revert to snapshot
                $cmdArgs = [];
                $cmdArgs[] = 'timeshift';
                $cmdArgs[] = '--restore';
                $cmdArgs[] = sprintf("--snapshot '%s'", $params['snapshot']);
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
}

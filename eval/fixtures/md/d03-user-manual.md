Sentinel start D03.

# Rayo Backup Agent — User Manual

Version 2.4, for Linux and Windows.

This manual explains how to install, configure and operate the Rayo Backup Agent, a small service that copies selected directories to a local disk, a network share or an object storage bucket on a schedule. It is written for system administrators who are comfortable with a command line. Each chapter starts with a short overview and then lists the procedures step by step.

![chart](img/chart.png)

The chart above shows typical backup durations for five dataset sizes on reference hardware.

## 1. Installation

The agent is distributed as a single binary with no runtime dependencies. It runs on 64-bit Linux distributions released after 2020 and on Windows 10 or later. Before you begin, make sure that the target machine has at least 200 MB of free disk space for the program and its catalogue, and that the account you use has permission to install system services.

### 1.1 Installing on Linux

1. Download the release archive for your architecture from the release page.
   - Use the `x86_64` archive for most servers.
   - Use the `aarch64` archive for ARM servers and single-board computers.
2. Verify the checksum of the archive against the published value.
3. Extract the archive and copy the binary into a directory on the `PATH`.
4. Register the service so that it starts automatically at boot.
   - On systemd distributions, use the unit file shipped in the archive.
   - On other systems, add the agent to the init scripts manually.
5. Start the service and confirm that it is running.

```bash
curl -fLO https://example.com/rayo-backup/2.4.0/rayo-backup-2.4.0-linux-x86_64.tar.gz
sha256sum rayo-backup-2.4.0-linux-x86_64.tar.gz
tar -xzf rayo-backup-2.4.0-linux-x86_64.tar.gz
sudo install -m 0755 rayo-backup /usr/local/bin/rayo-backup
sudo cp rayo-backup.service /etc/systemd/system/
sudo systemctl enable --now rayo-backup
systemctl status rayo-backup
```

> Note: The agent runs as an unprivileged user called `rayo` by default. The installer creates this user if it does not exist. Grant it read access to every directory you want to back up.

### 1.2 Installing on Windows

1. Download the Windows installer from the release page.
2. Run the installer as an administrator.
   - Accept the licence agreement.
   - Choose the installation directory, or keep the default.
3. Select whether the service should start automatically.
4. Finish the installer and open the Services console to confirm that "Rayo Backup Agent" is running.

The Windows installer also adds the command-line tool to the system `PATH`, so all commands in this manual work in PowerShell as well.

## 2. Configuration

The agent reads its settings from a single configuration file. On Linux the file is `/etc/rayo-backup/config.toml`; on Windows it is `C:\ProgramData\RayoBackup\config.toml`. The file is created with safe defaults during installation. Changes take effect after the service is reloaded.

### 2.1 General options

| Option | Default | Description |
| --- | --- | --- |
| `catalogue_dir` | `/var/lib/rayo-backup` | Directory where the agent keeps its index of backed-up files. |
| `log_level` | `info` | One of `error`, `warn`, `info` or `debug`. |
| `max_threads` | `4` | Number of files copied in parallel. |
| `bandwidth_limit` | `0` | Upload limit in KiB/s; `0` means unlimited. |
| `compression` | `zstd` | Compression method: `none`, `gzip` or `zstd`. |
| `retention_days` | `30` | Number of days to keep old versions of each file. |

### 2.2 Editing the configuration

1. Open the configuration file in a text editor with administrator rights.
2. Change the options you need.
   - Keep one option per line.
   - Quote values that contain spaces.
3. Check the file for errors with the built-in validator.
4. Reload the service.

```bash
sudo rayo-backup config check /etc/rayo-backup/config.toml
sudo systemctl reload rayo-backup
```

> Note: An invalid configuration is never applied. If the validator reports an error, the service keeps running with the previous settings and writes the error to the log.

## 3. Defining Backup Jobs

A backup job combines a set of source directories, a destination and a schedule. You can define as many jobs as you need, and each job runs independently. Jobs that share a destination are queued so that they do not compete for bandwidth.

### 3.1 Creating a job

1. Choose a short, unique name for the job.
2. List the source directories.
   - Use absolute paths.
   - Add exclusion patterns for caches and temporary files.
3. Choose a destination.
   - A local path such as `/mnt/backup`.
   - A network share such as `smb://nas/backups`.
   - An object storage bucket such as `s3://company-backups/servers`.
4. Set the schedule using a cron expression.
5. Save the job and run it once manually to check it.

```bash
rayo-backup job create home-dirs \
  --source /home \
  --exclude '*/.cache/*' \
  --dest s3://company-backups/servers/home \
  --schedule '30 2 * * *'
rayo-backup job run home-dirs --wait
```

### 3.2 Job options

| Option | Example | Meaning |
| --- | --- | --- |
| `--source` | `/srv/data` | Directory to back up; repeat for several directories. |
| `--exclude` | `*.tmp` | Glob pattern for files to skip. |
| `--dest` | `/mnt/backup` | Destination URL or path. |
| `--schedule` | `0 3 * * 0` | Cron expression in local time. |
| `--keep` | `14` | Number of versions to keep; overrides `retention_days`. |
| `--verify` | `true` | Read back each file after copying and compare checksums. |

## 4. Restoring Files

Restoring is the most important operation the agent performs, and it should be practised before it is needed. The agent can restore a single file, a directory or a complete job to its original location or to a different path. It never overwrites existing files unless you ask it to.

### 4.1 Restoring a directory

1. List the available snapshots for the job.
2. Pick the snapshot that predates the problem.
3. Restore into an empty directory first.
   - Check the restored files.
   - Compare them with the damaged originals if necessary.
4. Move the files into place once you are satisfied.

```bash
rayo-backup snapshot list home-dirs
rayo-backup restore home-dirs --snapshot 2026-10-31T02:30 \
  --path /home/alice/projects --target /tmp/restore
```

![photo](img/photo.png)

The photograph above shows the reference test server used for the restore benchmarks in this manual.

> Note: Restores from object storage may incur retrieval fees, depending on the storage class of the bucket. Check your provider’s pricing before restoring large amounts of data.

## 5. Monitoring

The agent records the result of every job in its catalogue and writes a summary line to the system log. You can query the status from the command line, export metrics to a monitoring system, or receive an e-mail when a job fails.

### 5.1 Checking job status

1. Run the status command to see the last result of every job.
2. Look for jobs marked `failed` or `overdue`.
   - A failed job ended with an error.
   - An overdue job did not start at its scheduled time.
3. Open the log for details about any problem.

```bash
rayo-backup status
journalctl -u rayo-backup --since today
```

### 5.2 Metrics

| Metric | Type | Description |
| --- | --- | --- |
| `rayo_job_duration_seconds` | gauge | Duration of the last run of each job. |
| `rayo_job_bytes_total` | counter | Bytes written per job since the service started. |
| `rayo_job_failures_total` | counter | Number of failed runs per job. |
| `rayo_catalogue_files` | gauge | Number of files tracked in the catalogue. |

Metrics are served on port 9464 when `metrics_listen` is set in the configuration file.

## 6. Troubleshooting

Most problems fall into one of a few categories: permissions, network access, storage space and configuration errors. The procedures below cover the most common cases. If a problem persists, collect the diagnostic bundle and contact support.

### 6.1 A job fails with "permission denied"

1. Identify the file named in the error message.
2. Check that the `rayo` user can read it.
   - Use `ls -l` to view the permissions.
   - Add the user to the owning group if appropriate.
3. Run the job again.

### 6.2 A job is slow

1. Check the bandwidth limit in the configuration.
2. Look at the destination’s load.
   - Network shares are often slow during office hours.
   - Object storage may throttle many small requests.
3. Increase `max_threads` gradually and measure the effect.

### 6.3 Collecting a diagnostic bundle

```bash
sudo rayo-backup diagnose --output /tmp/rayo-diagnose.tar.gz
```

The bundle contains the configuration with secrets removed, the last 1,000 log lines and the catalogue statistics. It does not contain any backed-up data.

> Note: Review the bundle before you send it. Although the agent removes passwords and access keys, file names in the log may still reveal information you consider confidential.

Sentinel end D03.

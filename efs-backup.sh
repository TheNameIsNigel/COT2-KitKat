#!/sbin/sh

EFS_PATH=`cat /etc/recovery.fstab | grep -v "#" | grep /efs | awk '{print $3}'`;

mkdir -p "$1"/cotrecovery/.efsbackup;

echo "">>"$1"/cotrecovery/.efsbackup/log.txt;
echo "Backup EFS ($EFS_PATH) to $1/cotrecovery/.efsbackup/efs.img">>"$1"/cotrecovery/.efsbackup/log.txt;
(cat "$EFS_PATH" > "$1"/cotrecovery/.efsbackup/efs.img) 2>> "$1"/cotrecovery/.efsbackup/log.txt;

if [ $? = 0 ];
     then echo "Success!">>$1/cotrecovery/.efsbackup/log.txt;
     else echo "Error!">>$1/cotrecovery/.efsbackup/log.txt;
fi;
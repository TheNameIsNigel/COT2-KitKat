#!/sbin/sh

EFS_PATH=`cat /etc/recovery.fstab | grep -v "#" | grep /efs | awk '{print $3}'`;

echo "">>"$1"/cotrecovery/.efsbackup/log.txt;
echo "Restore $1/cotrecovery/.efsbackup/efs.img to $EFS_PATH">>"$1"/cotrecovery/.efsbackup/log.txt;
(cat "$1"/cotrecovery/.efsbackup/efs.img > "$EFS_PATH") 2>> "$1"/cotrecovery/.efsbackup/log.txt;

if [ $? = 0 ];
     then echo "Success!">>"$1"/cotrecovery/.efsbackup/log.txt;
     else echo "Error!">>"$1"/cotrecovery/.efsbackup/log.txt;
fi;
debug .
host $env(PLDM_HOST)
xc xt0 zt0 on -tbrun
database -open pldm_db
probe -create -all -depth all tb_top -database pldm_db
xeset traceMemSize 50000
xeset triggerPos 2
run -swap
run
database -upload
xc off
exit

rm -rf bin

export PGUSER=postgres PGPASSWORD=temp123
psql -c "drop database test;"
for i in $(seq 0 15)
do
    psql -c "drop database test$i;"
done

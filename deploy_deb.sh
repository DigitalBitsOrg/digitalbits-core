
#!/bin/bash

echo "Create deb package..."

fpm -s dir -t deb -C /home/vpokhylo/digibits/digitalbits-core/src --name digitalbits-core --version 0.1.0 --iteration 1 --depends debian_dependency1 --description "Digitalbits-core" .

echo "deploying to Cloudsmith with cloudsmith-cli"

cd src/

pwd

ls

cloudsmith push deb digitalbits/dbtest/ubuntu/trusty digitalbits-core_0.1.0-1_amd64.deb

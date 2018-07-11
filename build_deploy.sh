#!/bin/bash

echo "Create deb package..."

fpm -s dir -t deb -C src --name digitalbits-core --version 0.1.0 --iteration 1 --depends debian_dependency1 --description "Digitalbits-core" .

echo "deploying to Cloudsmith with cloudsmith-cli"

cd src/

pwd

ls

cloudsmith push deb digitalbits/dbtest/ubuntu/trusty digitalbits-core_0.1.0-1_amd64.deb


echo "Create rpm package..."

fpm -s dir -t rpm -C src --name digitalbits-core --version 0.1.0 --iteration 1 --depends  redhat_dependency1 --description "digitalbits-core" .

echo "deploying to Cloudsmith with cloudsmith-cli"

cd src/

pwd

ls

cloudsmith push deb digitalbits/dbtest/centos/ digitalbits-core-0.1.0-1.x86_64.rpm

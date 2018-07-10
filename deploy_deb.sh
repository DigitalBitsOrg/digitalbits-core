
#!/bin/bash

echo "deploying to Cloudsmith with cloudsmith-cli"

cd src/

pwd

ls

cloudsmith push deb digitalbits/dbtest/ubuntu/trusty digitalbits-core_0.1.0-1_amd64.deb

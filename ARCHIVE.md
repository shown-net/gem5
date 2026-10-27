# Historical gem5 references

This branch retains the old commit graphs independently of the new v25 branches.
The checked-out source is the former calibration-v26 tree. Use the index below to inspect other snapshots.

## Calibration restart from v25 plus FST, 2026-10-27

The pre-restart development history is a parent of this archive update; it also
contains the previous deployment tip. These are historical implementations,
including TASK isolation, custom uop counters, ISA/FU calibration, frontend
groups and memory-service changes. The new active calibration branches start
from `74c1e6350bb91f581eb7fedfe9a5a154fc5f1ab0`, retain serial TSO as the
initial execution path, and do not inherit those calibrated mechanisms.

| Previous active reference | Commit |
| --- | --- |
| `pre-component-v25-20261027/calibration-dev` | `c43cd526e9a054c577557bd68a03bf313de1c9d5` |
| `pre-component-v25-20261027/calibration-deploy` | `e3b82f678b98aab63e6211693f1df1e399956517` |

Inspect a snapshot with `git show <commit>:<path>` from a history checkout.
The archive's checked-out v26 source remains unchanged.

| Original reference | Object | Commit |
| --- | --- | --- |
| `refs/heads/calibration` | `2ca9e9404479c55aadb3545ac706cfc5b6ec1932` | `2ca9e9404479c55aadb3545ac706cfc5b6ec1932` |
| `refs/heads/calibration-v25` | `6877f1c55337a45b5642d7e94932b9ebbf5d714e` | `6877f1c55337a45b5642d7e94932b9ebbf5d714e` |
| `refs/heads/calibration-v26` | `cefea43af27383e5b9704081bd83298a2675fd6e` | `cefea43af27383e5b9704081bd83298a2675fd6e` |
| `refs/heads/fastsim-v25` | `591c1e684a5766e0c1e18c854cc893ac65b6275f` | `591c1e684a5766e0c1e18c854cc893ac65b6275f` |
| `refs/heads/stable` | `fed0e07e930fecf1e73dce94dd44e69a5bf70c46` | `fed0e07e930fecf1e73dce94dd44e69a5bf70c46` |
| `refs/remotes/origin/calibration` | `2ca9e9404479c55aadb3545ac706cfc5b6ec1932` | `2ca9e9404479c55aadb3545ac706cfc5b6ec1932` |
| `refs/remotes/origin/calibration-v26` | `cefea43af27383e5b9704081bd83298a2675fd6e` | `cefea43af27383e5b9704081bd83298a2675fd6e` |
| `refs/remotes/origin/tao` | `9ab668a6890ad0cc7244080de60ba83771256463` | `9ab668a6890ad0cc7244080de60ba83771256463` |
| `refs/remotes/upstream/stable` | `fed0e07e930fecf1e73dce94dd44e69a5bf70c46` | `fed0e07e930fecf1e73dce94dd44e69a5bf70c46` |
| `refs/tags/archive/calibration-before-native-units-20261013` | `eafd9a9e914118f87fce07be0757cc97b6d69a16` | `eafd9a9e914118f87fce07be0757cc97b6d69a16` |
| `refs/tags/archive/calibration-pre-v2-20260928` | `4a5ecc2ab5825247b3a91fedcd874e0d2d12a503` | `aeca50ff51bfdccd3240af2822169c12b6296800` |
| `refs/tags/copyright_update` | `f5e36d156dc33a6c01c69c19efc2b5b0450496f6` | `f5e36d156dc33a6c01c69c19efc2b5b0450496f6` |
| `refs/tags/m5_1.0_beta1` | `73f78b9587a8b5645c7faf5ac1e258afbf1868db` | `73f78b9587a8b5645c7faf5ac1e258afbf1868db` |
| `refs/tags/m5_1.0_beta2` | `58fe84b6468f8157f096724a506885437d51c044` | `58fe84b6468f8157f096724a506885437d51c044` |
| `refs/tags/m5_1.0_tutorial` | `4cc9fbf61d70668a8d80681998a1db2b2e1b4a41` | `4cc9fbf61d70668a8d80681998a1db2b2e1b4a41` |
| `refs/tags/m5_1.0_web` | `5102958b8f5800523488730a5c86392790796f38` | `5102958b8f5800523488730a5c86392790796f38` |
| `refs/tags/m5_1.1` | `d6f2309dd7176903d6a7f901d38ab551c96dbdf5` | `d6f2309dd7176903d6a7f901d38ab551c96dbdf5` |
| `refs/tags/m5_2.0_beta1` | `a12dbc3074d505789aeeacd312e3a708d7a1f03c` | `a12dbc3074d505789aeeacd312e3a708d7a1f03c` |
| `refs/tags/m5_2.0_beta1_patch1` | `dc68512222596fdd81e62e685def20497f6e63d1` | `dc68512222596fdd81e62e685def20497f6e63d1` |
| `refs/tags/m5_2.0_beta2` | `b618e733bd580391702a844404417f9723b94588` | `b618e733bd580391702a844404417f9723b94588` |
| `refs/tags/m5_2.0_beta3` | `a8278c3bde2ba9abc2820afafa9d0e766e36b2c8` | `a8278c3bde2ba9abc2820afafa9d0e766e36b2c8` |
| `refs/tags/m5_2.0_beta4` | `d1fb5ad91e2429739b5a746fa36ab0ee822e33b7` | `d1fb5ad91e2429739b5a746fa36ab0ee822e33b7` |
| `refs/tags/m5_2.0_beta5` | `02a56d8d01ff4969f73740197309885e8ebdc527` | `02a56d8d01ff4969f73740197309885e8ebdc527` |
| `refs/tags/m5_2.0_beta6` | `9affd7adc64d16a7dd4733f5b90b2dfda5b1a279` | `9affd7adc64d16a7dd4733f5b90b2dfda5b1a279` |
| `refs/tags/show` | `9d5dad77cc85c35408b17eac13693ccb9cd46742` | `02510b5982466f06a798c4800c40cf3bcb08e2ea` |
| `refs/tags/stable_2012_02_02` | `94ce9712787c59720727890893ded8a533158e2d` | `94ce9712787c59720727890893ded8a533158e2d` |
| `refs/tags/stable_2012_06_28` | `c51fc5ceffd19c62c2895fb3444a532e9fe0f3ca` | `c51fc5ceffd19c62c2895fb3444a532e9fe0f3ca` |
| `refs/tags/stable_2013_06_16` | `c9e4678c16164ca12de951ea6faffc9ab9600b0b` | `c9e4678c16164ca12de951ea6faffc9ab9600b0b` |
| `refs/tags/stable_2013_10_14` | `8e54c93222cd69dd4c388adc37cded7b7eebef76` | `8e54c93222cd69dd4c388adc37cded7b7eebef76` |
| `refs/tags/stable_2014_02_15` | `a212844f6785f896b268b678f0018528c0ecfdc7` | `a212844f6785f896b268b678f0018528c0ecfdc7` |
| `refs/tags/stable_2014_08_26` | `0be64ffe2f4ff8824b3084362706ffbf456ea490` | `0be64ffe2f4ff8824b3084362706ffbf456ea490` |
| `refs/tags/stable_2014_12_14` | `f94fd44991ce51dd7f24364cac1ba060bf24e02d` | `f94fd44991ce51dd7f24364cac1ba060bf24e02d` |
| `refs/tags/v1.3.0-marker-window` | `9a2acab2d796512a66ca7b203ba7a47335af5eb3` | `c009b259020fa65421b56a9bcf3d0c6a0f5f9d52` |
| `refs/tags/v19.0` | `b1da3a42014daf7af29caac06e05f6fbf35e8175` | `9fc9c67b4242c03f165951775be5cd0812f2a705` |
| `refs/tags/v19.0.0.0` | `915557b7dd8426f526a99ac08adf8d1f8188bda1` | `9fc9c67b4242c03f165951775be5cd0812f2a705` |
| `refs/tags/v20.0` | `ebc7bd19887d54e198a3acb1f5a70f3ebde83e99` | `fa70478413e4650d0058cbfe81fd5ce362101994` |
| `refs/tags/v20.0.0.0` | `1d97623108e2c4673679413decf97900978c8dd0` | `b1b8af04439240c532d3530a02773b75b9853f77` |
| `refs/tags/v20.0.0.1` | `df08cecea35370fc3d13dbdf855a3139fbdfbf84` | `332a9de33db603e0aefedae1e05134db4257ea3e` |
| `refs/tags/v20.0.0.2` | `fa8052dee17bb35cc6dc0aa7913eaa19488a1b21` | `96fce476785a834f102ae69a895e661cf08e47cd` |
| `refs/tags/v20.0.0.3` | `2d25158eab1b1f04c50b49389834cb1ebe967e40` | `fa70478413e4650d0058cbfe81fd5ce362101994` |
| `refs/tags/v20.1` | `cf4a77cbcd7c491582bac4b60cc63c5ad9494dec` | `31cd81fdec46bae4b48d4f3788776936389dbdec` |
| `refs/tags/v20.1.0.0` | `76cc47a879e3d3880c0029d7ad0569d6d93d6303` | `090fa08c149768e940f857df9cb936da23ae48da` |
| `refs/tags/v20.1.0.1` | `35a63fab52e7942e5eae432bc0a45b2533d766b7` | `a265891e87833187e70d254a9ed09721cfce982c` |
| `refs/tags/v20.1.0.2` | `768b6f2fd3d8927f772fec19416784fd82d9497c` | `0d703041fcd5d119012b62287695723a2955b408` |
| `refs/tags/v20.1.0.3` | `df63e6eb7c2bbe31cd7d112ee21f833a80809b0f` | `cd21b5a5519940a0fa9b9a2dde68f30403d17f7e` |
| `refs/tags/v20.1.0.4` | `29f4daa421a4524ae0f81647add71769a90d75f4` | `2373934b8224e33c55ce96a0fe7751caeee13baf` |
| `refs/tags/v20.1.0.5` | `14cb9a80b98343b8047c3556ec99f308fc909dc8` | `31cd81fdec46bae4b48d4f3788776936389dbdec` |
| `refs/tags/v21.0` | `163565ae9eb04ad454c5f3c5888e51e076464e99` | `a85e48ceb7e8235d68292a65211b614f0c5343f9` |
| `refs/tags/v21.0.0.0` | `e803eaaa9790f8777bc10826cf192cca80648b43` | `ea7d012c00e5555857ef999b88a8ec2bde801a1f` |
| `refs/tags/v21.0.1.0` | `e2ae32b53bd9ef22af8cbe4311c3b9d82c305042` | `a85e48ceb7e8235d68292a65211b614f0c5343f9` |
| `refs/tags/v21.1` | `722b6981b499963d10331dc31115a83ecdae6a47` | `4666a4f9e31eae791ddabc619486c39bdecfabd3` |
| `refs/tags/v21.1.0.0` | `e4de91a1913413c53c12cdffb250c5544fcebd08` | `87c121fd954ea5a6e6b0760d693a2e744c2200de` |
| `refs/tags/v21.1.0.1` | `a6d572cf3e356f333e226526957208d2c8991b6c` | `082f0835b098887f0fdb89db908b59221b027686` |
| `refs/tags/v21.1.0.2` | `524f56b8810ef519a1be5487b57ddba33ae8b506` | `4666a4f9e31eae791ddabc619486c39bdecfabd3` |
| `refs/tags/v21.2` | `4b970cc8820140035febd37f2b6dc67bda4551f1` | `e4fae58da6c044b6efec62392ff99f343ce67947` |
| `refs/tags/v21.2.0.0` | `4a744d677a65ecb797019159269b8c7d648c970a` | `f554b1a7b56b5889bd5daec6e09eda8c3fbd93d1` |
| `refs/tags/v21.2.1.0` | `fa1dedcc71e297dde30ac1bebaf24b6cbd955f9b` | `141cc37c2d4b93959d4c249b8f7e6a8b2ef75338` |
| `refs/tags/v21.2.1.1` | `6273939c49d15aa0f12bff828ecd31381634d619` | `e4fae58da6c044b6efec62392ff99f343ce67947` |
| `refs/tags/v22.0` | `75cba43e91414993ccad7edbd14aac7fadfaf18f` | `1d03f6de941520860c673b5f7954c82a46e8b191` |
| `refs/tags/v22.0.0.0` | `b2dbfb96b7d9a8263276acf570ef1bd6a397d1f7` | `7b9364b5c0e0a211449c2974c31f9c5a4176557d` |
| `refs/tags/v22.0.0.1` | `dc16d8783e63beb79fdc89b886176cb78ac56e20` | `39f85b7a3be1ee0ff6e375c9791dd62d23eb8a3e` |
| `refs/tags/v22.0.0.2` | `38ff8f01c644227ab90c315c5b1c7dce4125c908` | `1d03f6de941520860c673b5f7954c82a46e8b191` |
| `refs/tags/v22.1` | `a35e853ff456988cf07d886ae7cd29f74318fb34` | `263e7206aa719421582aff69f66943541cc3a744` |
| `refs/tags/v22.1.0.0` | `ec47a01dfb55d3fcd06bc129eec880ee15b230b3` | `263e7206aa719421582aff69f66943541cc3a744` |
| `refs/tags/v23.0` | `03532879770d0b9ba9d8583e554e40d633d1be54` | `e03d68370569ab631b453fbca11357b6cba3a8ec` |
| `refs/tags/v23.0.0.0` | `bb50325f97f2f92e87be305d118b68e83934dd66` | `d77a181f1ac2df59c7ca5492aa3b605313531039` |
| `refs/tags/v23.0.0.1` | `9ce8a945bec5bfb67c8c00b05d49e55492a055e9` | `fca6c16d1392a619c128513cc63f6fb84fefe3ed` |
| `refs/tags/v23.0.1.0` | `918e047b5870e69e55068c95ce13c35e75c32f2a` | `e03d68370569ab631b453fbca11357b6cba3a8ec` |
| `refs/tags/v23.1` | `23d7a2a2462ad33de07d9f250aed12cb9e5aeae0` | `03b3a423c2977c7799f0a84f605b17ef41577aac` |
| `refs/tags/v23.1.0.0` | `8cf39f4ab44778a6e80743efb6962d98490ddfce` | `03b3a423c2977c7799f0a84f605b17ef41577aac` |
| `refs/tags/v24.0` | `261aca1d0e5d20cb6adde581c33f6f06c55c3b0b` | `895495e776cce9dd139a4fe929b52e4f3f63061c` |
| `refs/tags/v24.0.0.0` | `70ceaf180cc16c859c36ef6a7a739f1b270ea1d2` | `481882a5b7a4e13892f06efca76095524c868f90` |
| `refs/tags/v24.0.0.1` | `f973d08a3dcde582e53a2197614820c41dca2d49` | `895495e776cce9dd139a4fe929b52e4f3f63061c` |
| `refs/tags/v24.1` | `1b44f15154cb7fe85087102bcd7a320e534ef1b9` | `54f73df036e68426ac4eabff498f121162d7e6ac` |
| `refs/tags/v24.1.0.0` | `d476a7d1df020e9ef08167d538d95db2fae74869` | `6006fec04e33371c1023a00d6591f1ad2cbc2a0c` |
| `refs/tags/v24.1.0.1` | `9b2ea756f3dae5486fdddbab3c8e23dc915c8529` | `02510b5982466f06a798c4800c40cf3bcb08e2ea` |
| `refs/tags/v24.1.0.2` | `5517410573d7fdc4d4aeb23173f943743460b9a8` | `17b53b95ef0a5b8a3daf568fa991e4ccb7d8ea03` |
| `refs/tags/v24.1.0.3` | `30201345dbddc4e0fb59f22290f7fad4bfc4f67f` | `54f73df036e68426ac4eabff498f121162d7e6ac` |
| `refs/tags/v25.0` | `b2cad5e47d5a79f2cafa24dc11a54a40ce93246b` | `4e6923d243ed2f08379f1b14a148300a65b15cf8` |
| `refs/tags/v25.0.0.0` | `b53a9f00a942fbe4f17c4106d7f0272cf839c27e` | `8293c838fb0409e73dfa6c6a580a8f7b3d5e3a01` |
| `refs/tags/v25.0.0.1` | `b4333bd08b5e5d9a438f1de6c15e871eb3044b5b` | `4e6923d243ed2f08379f1b14a148300a65b15cf8` |
| `refs/tags/v25.1` | `46cee3aa36a1412eb0ffca0102b599e55452ba45` | `eb164cf50981bfb960cba4db769fab7b40b8ec03` |
| `refs/tags/v25.1.0.0` | `4f32ba318ca930d036c41f0c6bc5648d5c197b67` | `eb164cf50981bfb960cba4db769fab7b40b8ec03` |
| `refs/tags/v25.1.0.1` | `fb4f9f7fbd56ff50325c52704ffab704c6d85a09` | `fb4f9f7fbd56ff50325c52704ffab704c6d85a09` |
| `refs/tags/v26.0.0.0` | `45770857fc792fbea05d5dcea61fab66553bd9a2` | `fed0e07e930fecf1e73dce94dd44e69a5bf70c46` |
| `remote-live/origin-tags/v1.3.0-marker-window` | `9a2acab2d796512a66ca7b203ba7a47335af5eb3` | `c009b259020fa65421b56a9bcf3d0c6a0f5f9d52` |
| `remote-live/origin/calibration` | `2ca9e9404479c55aadb3545ac706cfc5b6ec1932` | `2ca9e9404479c55aadb3545ac706cfc5b6ec1932` |
| `remote-live/origin/calibration-v26` | `cefea43af27383e5b9704081bd83298a2675fd6e` | `cefea43af27383e5b9704081bd83298a2675fd6e` |
| `remote-live/origin/tao` | `9ab668a6890ad0cc7244080de60ba83771256463` | `9ab668a6890ad0cc7244080de60ba83771256463` |

## Source-only initialization superseded by official v25 ancestry

These references were deployed before restoring the official v25 parent.

| Previous reference | Commit |
| --- | --- |
| `pre-v25-ancestry/fastsim-v25` | `2f6bec5331b6399f2dbdcc280908661e836104ee` |
| `pre-v25-ancestry/calibration-v25` | `842c7427bb3066fe90a4f30af0fb0f83139986f6` |
| `pre-v25-ancestry/calibration-dev` | `6e7d34bd4537888efa982a6a8fba7332141c805c` |

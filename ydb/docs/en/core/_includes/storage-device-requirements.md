For YDB to work efficiently, we recommend using physical (not virtual) disks larger than 800 GB as block devices.

The minimum disk size must be at least 80 GB, otherwise the node won't be able to use the block store volume. Correct and uninterrupted operation with minimum-size disks is not guaranteed. We recommend using such disks exclusively for informational purposes.

{% note warning %}

Configurations with disks less than 800 GB or any types of storage system virtualization cannot be used for production services or system performance testing.

{% endnote %}

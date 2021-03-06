darktable
=========

darktable is an open source photography workflow application and raw developer. A virtual lighttable and darkroom for photographers. It manages your digital negatives in a database, lets you view them through a zoomable lighttable and enables you to develop raw images and enhance them.

[http://www.darktable.org/](http://www.darktable.org/ "darktable homepage")

Contributing
------------

* Write a blog about darktable
* Create a tutorial for darktable
* Help expand the [user wiki](http://www.darktable.org/redmine/projects/users/wiki)
* Answer questions on the [user mailing list](http://news.gmane.org/gmane.comp.graphics.darktable.user)
* Share your ideas on the [developer mailing list](http://news.gmane.org/gmane.comp.graphics.darktable.devel)
* Test [releases](http://www.darktable.org/install/)
* Review [pull requests](http://github.com/darktable-org/darktable/pulls)
* Start [hacking on darktable](http://www.darktable.org/redmine/projects/darktable/wiki/Contributing_code)

Building
--------

### Easy way

```bash
./build.sh --prefix /opt/darktable --build-type Release
```

### Manual way

```bash
mkdir build/
cd build/
cmake -DCMAKE_INSTALL_PREFIX=/opt/darktable/ ..
make
sudo make install
```

### Further reading

There is a [comprehensive list](http://redmine.darktable.org/projects/darktable/wiki/Building_darktable_20) of build instructions for all the widely used Linux distributions.


**Tip:** Check that you have the latest [gphoto2 library](http://www.gphoto.org/ "gphoto2 homepage") installed in order to support the newest cameras.

Issue tracking
--------------

[http://www.darktable.org/redmine/projects/darktable/issues](http://www.darktable.org/redmine/projects/darktable/issues "darktable issue tracking")

Wiki
----

* [User wiki](http://www.darktable.org/redmine/projects/users/wiki "darktable user wiki")
* [Developer wiki](http://www.darktable.org/redmine/projects/darktable/wiki "darktable developer wiki")


Mailing lists
-------------

* Users [[subscribe](mailto:darktable-user+subscribe@lists.darktable.org) | [archive](http://news.gmane.org/gmane.comp.graphics.darktable.user)]
* Developer [[subscribe](mailto:darktable-dev+subscribe@lists.darktable.org) | [archive](http://news.gmane.org/gmane.comp.graphics.darktable.devel)]

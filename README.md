# aquabsd-init

Replacement for the Research Unix-style init system on aquaBSD.
`init` is the first process launched on the system, and is the one which is at the root of all subsequent processes.
It launches, manages, and maintains background services (e.g. `sshd`, `ntpd`, `dhclient`).

It also launches the AQUA WM, keeps the Pat Shack repository up to date (through the `patd` daemon), and handles AQUA app permissions.

## Services & dependencies

Services can either be system-wide (located in `/etc/init/services` or `/usr/local/etc/init/services` for third-party packages) or user-specific (located in `~/.local/etc/init/services` - these services are started with the relevant user permissions).
Each service is a directory containing a `desc.json` file, which tells `init` information about the service in question (notably its name, dependencies, and what actions to take), and some other private files the service has access to.
Here is an example `desc.json` file:

```json
{
	"name": "sshd", // colloquial name, displayed to the user
	"desc": "SSH daemon",

	"exec": "/usr/sbin/sshd",
	"stop": "bin/stop",

	// custom commands provided by the service

	"cmds": {
		"sanity": {
			"desc": "Perform sanity check on configuration.",
			"exec": "bin/sanity"
		},

		"keygen": {
			"desc": "Generate host keys.",
			"exec": "bin/keygen"
		},
	},

	"deps": [ "LOGIN", "FILESYSTEM" ], // other services the service depends on
	"before": [] // which services must the service absolutely finish before
}
```

(Similar to `/etc/rc.d/sshd` on FreeBSD.)

`init` starts off by reading all these `desc.json` files for each service, and then building a graph out of them.
This graph can be exported as a GraphViz file by running:

```sh
% service -g
```

### Third party services

Care must be taken so that no two services directories have the same name, so third-party packages which may need to install additional services to the system are encouraged to:

- Install services into `/usr/local/etc/init/services` instead of `/etc/init/services`. This is so that there's absolutely no chance of a naming conflict with the base system.
- Name services and service directories in a unique way (e.g., `package_name.service_version.service_name` - this is however not a strict requirement by aquaBSD). A friendlier name may be used as the `name` member in `desc.json` though.

### Dummy services

It is sometimes useful to have so-called "dummy" services to act as layers of abstraction above groups of related services.
They are typically denoted by uppercase letters, but this is not a strict requirement by aquaBSD.
Here is an example `desc.json` file:

```json
{
	"name": "NETWORKING",
	"desc": "Dummy service for services which require networking to be operational before starting.",

	"deps": [ "netif", "netwait", "netoptions", "routing", "ppp", "ipfw", "stf", "defaultroute", "route6d", "resolv", "bridge", "static_arp", "static_ndp" ]
}
```

(Similar to `/etc/rc.d/NETWORKING` on FreeBSD.)

### `/etc/rc` compatibility

The `init` found on versions of Research Unix and BSD usually runs a script located at `/etc/rc`, which in turn runs services as other scripts in `/etc/rc.d` and `/usr/local/etc/rc.d`.
These scripts will be parsed and read just as before, as many packages still assume this init system.
On NetBSD & FreeBSD, the `rcorder` utility was used to resolve dependencies between these; in aquaBSD, this utility no longer exists in the base system, and its functionality has been taken over by `init`.

The `/etc/rc.conf` configuration file from the NetBSD & FreeBSD init systems is ignored.
Some of the configuration options present in there have been moved over to `sysctl` OID's.
Please don't take this as a cue to put your configurations in `/boot/loader.conf` though, that's a bad idea 😉

## `libinit`

A library called `libinit` is available to programs, which provides an interface for interacting with the init system and services. E.g., given the correct user permissions, it can restart services or communicate with them.
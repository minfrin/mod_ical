# RPM Spec file for mod_ical

Name:      mod_ical
Version:   0.0.6
Release:   1%{?dist}
Summary:   Apache iCal/jCal/xCal filter module
License:   ASL 2.0
Group:     System Environment/Daemons
Source:    https://github.com/minfrin/%{name}/releases/download/%{name}-%{version}/%{name}-%{version}.tar.bz2
BuildRequires: gcc, pkgconfig(apr-1), pkgconfig(apr-util-1), httpd-devel, pkgconfig(libical), pkgconfig(json-c), pkgconfig(libxml-2.0)
Requires: httpd

%description
The Apache mod_ical module provides a set of filters to
filter RFC5545 iCalendar data and convert it to
RFC6321 xCal / RFC7265 jCal.

%prep
%setup -q
%build
%configure
%make_build

%install
%make_install

%files
%{_libdir}/httpd/modules/mod_ical.so

%doc AUTHORS ChangeLog README.md
%license COPYING

%changelog
* Sun May 22 2016 Graham Leggett <minfrin@sharp.fm> - 0.0.6-1
- Initial version of the package


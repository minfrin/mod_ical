# RPM Spec file for mod_ical

Name:      mod_ical
Version:   0.0.6
Release:   1%{?dist}
Summary:   Apache mod_ical module
License:   ASL v2.0
Group:     System Environment/Daemons
Source:    https://github.com/minfrin/%{name}/releases/download/%{name}-%{version}/%{name}-%{version}.tar.bz2
BuildRequires: pkgconfig(apr-1), pkgconfig(apr-util-1), httpd-devel, libical-devel, json-c-devel, libxml2-devel
Requires: httpd

%description
The Apache mod_ical module provides a set of filters to
filter iCalendar data and convert it to xCal/jCal.

%prep
%setup -q
%build
%configure
%make_build

%install
%make_install

%files
%{_libdir}/httpd/modules/mod_ical.so

%doc AUTHORS ChangeLog NEWS README
%license COPYING

%changelog
* Sun May 22 2016 Graham Leggett <minfrin@sharp.fm> - 0.0.6-1
- Initial version of the package

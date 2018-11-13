import { Component, OnDestroy, OnInit, TemplateRef, ViewChild } from '@angular/core';
import { Subscription } from 'rxjs';

import { RbdMirroringService } from '../../../../shared/api/rbd-mirroring.service';
import { CephShortVersionPipe } from '../../../../shared/pipes/ceph-short-version.pipe';

@Component({
  selector: 'cd-mirroring-daemons',
  templateUrl: './daemon-list.component.html',
  styleUrls: ['./daemon-list.component.scss']
})
export class DaemonListComponent implements OnInit, OnDestroy {
  @ViewChild('healthTmpl')
  healthTmpl: TemplateRef<any>;

  subs: Subscription;

  data: [];
  columns: {};

  constructor(
    private rbdMirroringService: RbdMirroringService,
    private cephShortVersionPipe: CephShortVersionPipe
  ) {}

  ngOnInit() {
    this.columns = [
      { prop: 'instance_id', name: 'Instance', flexGrow: 2 },
      { prop: 'id', name: 'ID', flexGrow: 2 },
      { prop: 'server_hostname', name: 'Hostname', flexGrow: 2 },
      {
        prop: 'version',
        name: 'Version',
        pipe: this.cephShortVersionPipe,
        flexGrow: 2
      },
      {
        prop: 'health',
        name: 'Health',
        cellTemplate: this.healthTmpl,
        flexGrow: 1
      }
    ];

    this.subs = this.rbdMirroringService.subscribe((data: any) => {
      if (!data) {
        return;
      }
      this.data = data.content_data.daemons;
    });
  }

  ngOnDestroy(): void {
    this.subs.unsubscribe();
  }

  refresh() {
    this.rbdMirroringService.refresh();
  }
}

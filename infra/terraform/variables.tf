variable "region" {
  description = "AWS region to deploy into."
  type        = string
  default     = "us-east-1"
}

variable "cluster_name" {
  description = "Name of the EKS cluster."
  type        = string
  default     = "matchbox"
}

variable "kubernetes_version" {
  description = "EKS control plane version."
  type        = string
  default     = "1.31"
}

variable "node_instance_type" {
  description = <<-EOT
    Worker node size. 2 vCPU / 8 GiB is the smallest that comfortably holds this workload on
    two nodes: the Kafka StatefulSet requests 500m/1Gi, four engine pods request 250m/256Mi
    each, and a trimmed kube-prometheus-stack needs roughly another 1 vCPU / 2.5 GiB on top
    of the ~0.6 vCPU the EKS system pods already take per node.

    m7i-flex.large rather than the obvious t3.large: a new AWS account starts on the Free
    Tier plan, which refuses to launch any instance type not marked free-tier-eligible, and
    t3.large is not one. The node group fails with AsgInstanceLaunchFailures /
    InvalidParameterCombination after ~30 minutes of retrying. m7i-flex.large has the same
    2 vCPU / 8 GiB and is eligible. Check with:
      aws ec2 describe-instance-types --filters Name=free-tier-eligible,Values=true
  EOT
  type        = string
  default     = "m7i-flex.large"
}

variable "node_count" {
  description = <<-EOT
    Two nodes, so a single node failure does not take the whole demo with it, and so the
    Kafka pod and the observability stack are not forced onto the same host. Fixed size,
    not autoscaled: this cluster exists for a short evidence-capture session.
  EOT
  type        = number
  default     = 2
}

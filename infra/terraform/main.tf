# AWS infrastructure only: VPC, EKS control plane, one managed node group, and the IAM/OIDC
# wiring EKS needs. Kafka, the engine service, and the observability stack are Kubernetes
# objects and are applied afterwards by infra/k8s/deploy-eks.sh.
#
# The split is deliberate. Pointing Terraform's Kubernetes and Helm providers at a cluster
# that the same apply is still creating means provider configuration that depends on
# resources not yet in state - fragile ordering, and a failure mode that leaves state half
# applied. Phase 4 already proved out a plain, inspectable deploy script for the app layer,
# so this extends that instead of inventing a second mechanism.

data "aws_availability_zones" "available" {
  state = "available"
}

locals {
  # Two AZs: the minimum EKS will accept. More would only add cross-AZ data transfer cost
  # for a cluster that lives for one session.
  azs = slice(data.aws_availability_zones.available.names, 0, 2)

  tags = {
    Project = "matchbox"
  }
}

module "vpc" {
  source  = "terraform-aws-modules/vpc/aws"
  version = "~> 5.13"

  name = "${var.cluster_name}-vpc"
  cidr = "10.0.0.0/16"

  azs             = local.azs
  private_subnets = ["10.0.1.0/24", "10.0.2.0/24"]
  public_subnets  = ["10.0.101.0/24", "10.0.102.0/24"]

  # Nodes sit in private subnets and reach the internet through NAT, which is what lets
  # them pull images and register with the control plane without public IPs.
  enable_nat_gateway = true

  # One NAT gateway for the whole VPC rather than one per AZ. NAT is billed hourly whether
  # or not traffic flows and is the classic forgotten cost in a torn-down-late demo; a
  # portfolio cluster does not need per-AZ NAT redundancy.
  single_nat_gateway = true

  enable_dns_hostnames = true

  # Tags the AWS load balancer controller and EKS itself look for when placing resources.
  public_subnet_tags = {
    "kubernetes.io/role/elb" = 1
  }
  private_subnet_tags = {
    "kubernetes.io/role/internal-elb" = 1
  }

  tags = local.tags
}

module "eks" {
  source  = "terraform-aws-modules/eks/aws"
  version = "~> 20.31"

  cluster_name    = var.cluster_name
  cluster_version = var.kubernetes_version

  # Public endpoint so kubectl and helm can reach the cluster from a laptop without a
  # bastion or VPN. Acceptable for an ephemeral demo; a real deployment would restrict this
  # to known CIDRs or keep it private entirely.
  cluster_endpoint_public_access = true

  vpc_id     = module.vpc.vpc_id
  subnet_ids = module.vpc.private_subnets

  # Grants the identity running Terraform cluster-admin, so the deploy script can use
  # kubectl immediately after apply without a separate aws-auth ConfigMap step.
  enable_cluster_creator_admin_permissions = true

  # CoreDNS, kube-proxy and the VPC CNI are required for pods to schedule and resolve at
  # all; EBS CSI is what makes the Kafka StatefulSet's PersistentVolumeClaim bind, which
  # would otherwise sit Pending forever on a cluster that has no default provisioner.
  cluster_addons = {
    coredns    = {}
    kube-proxy = {}
    vpc-cni    = {}
    aws-ebs-csi-driver = {
      service_account_role_arn = module.ebs_csi_irsa.iam_role_arn
    }
  }

  eks_managed_node_groups = {
    default = {
      instance_types = [var.node_instance_type]
      min_size       = var.node_count
      max_size       = var.node_count
      desired_size   = var.node_count

      # On-demand rather than spot: a spot reclaim partway through the load window would
      # corrupt the very measurements this cluster exists to capture, and the saving over a
      # sub-hour session is cents.
      capacity_type = "ON_DEMAND"
    }
  }

  tags = local.tags
}

# The EBS CSI driver needs its own IAM role assumed via the cluster's OIDC provider, or
# PersistentVolumeClaims never bind.
module "ebs_csi_irsa" {
  source  = "terraform-aws-modules/iam/aws//modules/iam-role-for-service-accounts-eks"
  version = "~> 5.44"

  role_name             = "${var.cluster_name}-ebs-csi"
  attach_ebs_csi_policy = true

  oidc_providers = {
    main = {
      provider_arn               = module.eks.oidc_provider_arn
      namespace_service_accounts = ["kube-system:ebs-csi-controller-sa"]
    }
  }

  tags = local.tags
}
